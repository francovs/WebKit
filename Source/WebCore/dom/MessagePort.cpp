/*
 * Copyright (C) 2008-2023 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include "config.h"
#include "MessagePort.h"

#include "ContextDestructionObserverInlines.h"
#include "Document.h"
#include "EventNames.h"
#include "ExceptionOr.h"
#include "JSDOMGlobalObject.h"
#include "Logging.h"
#include "MessageEvent.h"
#include "MessagePortChannelProvider.h"
#include "MessageWithMessagePorts.h"
#include "StructuredSerializeOptions.h"
#include "WebCoreOpaqueRoot.h"
#include "WorkerGlobalScope.h"
#include "WorkerThread.h"
#include <JavaScriptCore/TopExceptionScope.h>
#include <wtf/CompletionHandler.h>
#include <wtf/Lock.h>
#include <wtf/Scope.h>
#include <wtf/TZoneMallocInlines.h>

namespace WebCore {

WTF_MAKE_TZONE_ALLOCATED_IMPL(MessagePort);

static Lock allMessagePortsLock;
static HashMap<MessagePortIdentifier, ThreadSafeWeakPtr<MessagePort>>& NODELETE allMessagePorts() WTF_REQUIRES_LOCK(allMessagePortsLock)
{
    static NeverDestroyed<HashMap<MessagePortIdentifier, ThreadSafeWeakPtr<MessagePort>>> map;
    return map;
}

static HashMap<MessagePortIdentifier, ScriptExecutionContextIdentifier>& NODELETE portToContextIdentifier() WTF_REQUIRES_LOCK(allMessagePortsLock)
{
    static NeverDestroyed<HashMap<MessagePortIdentifier, ScriptExecutionContextIdentifier>> map;
    return map;
}

void MessagePort::setMessageHandler(MessageHandler&& messageHandler)
{
    ASSERT(!m_messageHandler);
    m_messageHandler = WTF::move(messageHandler);
    start();
}

bool MessagePort::isMessagePortAliveForTesting(const MessagePortIdentifier& identifier)
{
    Locker locker { allMessagePortsLock };
    return allMessagePorts().contains(identifier);
}

void MessagePort::notifyMessageAvailable(const MessagePortIdentifier& identifier)
{
    ASSERT(isMainThread());
    std::optional<ScriptExecutionContextIdentifier> scriptExecutionContextIdentifier;
    ThreadSafeWeakPtr<MessagePort> weakPort;
    {
        Locker locker { allMessagePortsLock };
        scriptExecutionContextIdentifier = portToContextIdentifier().getOptional(identifier);
        weakPort = allMessagePorts().get(identifier);
    }
    if (!scriptExecutionContextIdentifier)
        return;

    ScriptExecutionContext::ensureOnContextThread(*scriptExecutionContextIdentifier, [weakPort = WTF::move(weakPort)](auto&) {
        if (RefPtr port = weakPort.get())
            port->messageAvailable();
    });
}

Ref<MessagePort> MessagePort::create(ScriptExecutionContext& scriptExecutionContext, const MessagePortIdentifier& local, const MessagePortIdentifier& remote)
{
    Ref messagePort = adoptRef(*new MessagePort(scriptExecutionContext, local, remote));
    messagePort->suspendIfNeeded();
    return messagePort;
}

MessagePort::MessagePort(ScriptExecutionContext& scriptExecutionContext, const MessagePortIdentifier& local, const MessagePortIdentifier& remote)
    : ActiveDOMObject(&scriptExecutionContext)
    , m_identifier(local)
    , m_remoteIdentifier(remote)
    , m_contextIdentifier(scriptExecutionContext.identifier())
{
    LOG(MessagePorts, "Created MessagePort %s (%p) in process %" PRIu64, m_identifier.logString().utf8().data(), this, Process::identifier().toUInt64());

    Locker locker { allMessagePortsLock };
    // We disable threading assertions since the allMessagePorts() is used from multiple threads in a safe way, using a lock.
    allMessagePorts().set(m_identifier, ThreadSafeWeakPtr { *this });
    portToContextIdentifier().set(m_identifier, scriptExecutionContext.identifier());

    // Make sure the WeakPtrFactory gets initialized eagerly on the thread the MessagePort gets constructed on for thread-safety reasons.
    EventTarget::initializeWeakPtrFactory();

    scriptExecutionContext.createdMessagePort(*this);

    // Don't need to call processMessageWithMessagePortsSoon() here, because the port will not be opened until start() is invoked.
}

MessagePort::~MessagePort()
{
    LOG(MessagePorts, "Destroyed MessagePort %s (%p) in process %" PRIu64, m_identifier.logString().utf8().data(), this, Process::identifier().toUInt64());

    Locker locker { allMessagePortsLock };

    auto iterator = allMessagePorts().find(m_identifier);
    if (iterator != allMessagePorts().end()) {
        // ThreadSafeWeakPtr::get() returns null as soon as the object has started destruction.
        if (RefPtr messagePort = iterator->value.get(); !messagePort) {
            allMessagePorts().remove(iterator);
            portToContextIdentifier().remove(m_identifier);
        }
    }

    if (m_entangled)
        close();

    if (RefPtr context = scriptExecutionContext())
        context->destroyedMessagePort(*this);
}

ExceptionOr<void> MessagePort::postMessage(JSC::JSGlobalObject& globalObject, JSC::JSValue messageValue, StructuredSerializeOptions&& options)
{
    LOG(MessagePorts, "Attempting to post message to port %s (to be received by port %s)", m_identifier.logString().utf8().data(), m_remoteIdentifier.logString().utf8().data());

    Vector<Ref<MessagePort>> ports;
    auto messageData = SerializedScriptValue::create(globalObject, messageValue, WTF::move(options.transfer), ports, SerializationForStorage::No);
    if (messageData.hasException())
        return messageData.releaseException();

    if (!isEntangled())
        return { };
    ASSERT(scriptExecutionContext());

    Vector<TransferredMessagePort> transferredPorts;
    // Make sure we aren't connected to any of the passed-in ports.
    if (!ports.isEmpty()) {
        for (auto& port : ports) {
            if (port->identifier() == m_identifier || port->identifier() == m_remoteIdentifier)
                return Exception { ExceptionCode::DataCloneError };
        }

        // FIXME: update disentanglePorts()
        auto disentangleResult = MessagePort::disentanglePorts(WTF::move(ports));
        if (disentangleResult.hasException())
            return disentangleResult.releaseException();
        transferredPorts = disentangleResult.releaseReturnValue();
    }

    MessageWithMessagePorts message { messageData.releaseReturnValue(), WTF::move(transferredPorts) };

    LOG(MessagePorts, "Actually posting message to port %s (to be received by port %s)", m_identifier.logString().utf8().data(), m_remoteIdentifier.logString().utf8().data());

    MessagePortChannelProvider::singleton().postMessageFromPort(WTF::move(message), identifier());
    return { };
}

ExceptionOr<void> MessagePort::postMessage(JSC::JSGlobalObject& globalObject, JSC::JSValue messageValue, Vector<JSC::Strong<JSC::JSObject>>&& transfer)
{
    return postMessage(globalObject, messageValue, StructuredSerializeOptions { WTF::move(transfer) });
}

TransferredMessagePort MessagePort::disentangle()
{
    ASSERT(m_entangled);
    m_entangled = false;

    Ref context = *scriptExecutionContext();
    //protect(MessagePortChannelProvider::fromContext(context))->messagePortDisentangled(m_identifier);
    MessagePortChannelProvider::singleton().disentangleForShipping(m_identifier);

    // We can't receive any messages or generate any events after this, so remove ourselves from the list of active ports.
    context->destroyedMessagePort(*this);
    context->willDestroyActiveDOMObject(*this);
    context->willDestroyDestructionObserver(*this);

    observeContext(nullptr);

    return { identifier(), remoteIdentifier() };
}

// Invoked to notify us that there are messages available for this port.
// This code may be called from another thread, and so should not call any non-threadsafe APIs (i.e. should not call into the entangled channel or access mutable variables).
void MessagePort::messageAvailable()
{
    // This MessagePort object might be disentangled because the port is being transferred,
    // in which case we'll notify it that messages are available once a new end point is created.
    RefPtr context = scriptExecutionContext();
    if (!context || context->activeDOMObjectsAreSuspended())
        return;

    context->processMessageForPortSoon(m_identifier, [pendingActivity = makePendingActivity(*this)] { });
}

void MessagePort::start()
{
    // Do nothing if we've been cloned or closed.
    if (!isEntangled())
        return;

    ASSERT(scriptExecutionContext());
    if (m_started)
        return;

    m_started = true;
    MessagePortChannelProvider::singleton().startPort(*this);
    // protect(scriptExecutionContext())->processMessageForPortSoon(m_identifier, [pendingActivity = makePendingActivity(*this)] { });
    
}

void MessagePort::stop()
{
    close();
}

void MessagePort::close()
{
    if (m_state == State::Unregistered) {
        m_isDetached = true;
        return;
    }

    if (m_isDetached)
        return;
    m_isDetached = true;

    MessagePortChannelProvider::singleton().closePort(identifier());

    removeAllEventListeners();
    m_messageHandler = { };
}

void MessagePort::contextDestroyed()
{
    ASSERT(scriptExecutionContext());

    close();
    ActiveDOMObject::contextDestroyed();
}

void MessagePort::scheduleHandlingForMessages(size_t count)
{
    // FIXME: avoid thread-hopping.
    ScriptExecutionContext::postTaskTo(m_contextIdentifier, [weakThis = ThreadSafeWeakPtr<MessagePort>(*this), count](auto& context) {
        for (size_t i=0; i<count; ++i) {
            context.eventLoop().queueTask(TaskSource::PostedMessageQueue, [weakThis = weakThis]() {
                if (RefPtr port = weakThis.get())
                    port->processOneMessage();
            });
        }
    });
}

void MessagePort::processOneMessage()
{
    ASSERT(started());

    RefPtr context = scriptExecutionContext();
    if (!context || context->activeDOMObjectsAreSuspended() || !isEntangled())
        return;

    ASSERT(context->isContextThread());
    auto* globalObject = context->globalObject();
    Ref vm = globalObject->vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);

    if (RefPtr workerGlobalScope = dynamicDowncast<WorkerGlobalScope>(*context))
        if (workerGlobalScope->isClosing())
            return;


    // FIXME: implement suspension then resume test
    // 
    // Not anymore: the event loop logic takes care of this!
    auto message = MessagePortChannelProvider::singleton().takeOneMessage(identifier());

    if (m_messageHandler) {
        ASSERT(message.transferredPorts.isEmpty());
        m_messageHandler(*downcast<JSDOMGlobalObject>(globalObject), message.message.releaseNonNull().get());
        return;
    }

    Vector<Ref<MessagePort>> receivedPorts = MessagePortChannelProvider::singleton().claimShippedPorts(*context, message.transferredPorts);
    
    auto event = MessageEvent::create(*globalObject, message.message.releaseNonNull(), { }, { }, { }, WTF::move(receivedPorts));
    if (scope.exception()) [[unlikely]] {
        // Currently, we assume that the only way we can get here is if we have a termination.
        RELEASE_ASSERT(vm->hasPendingTerminationException());
        return;
    }
    dispatchEvent(event.event);
}

void MessagePort::dispatchEvent(Event& event)
{
    if (m_isDetached)
        return;

    if (RefPtr globalScope = dynamicDowncast<WorkerGlobalScope>(scriptExecutionContext())) {
        if (globalScope->isClosing())
            return;
    }

    EventTarget::dispatchEvent(event);
}

// https://html.spec.whatwg.org/multipage/web-messaging.html#ports-and-garbage-collection
bool MessagePort::virtualHasPendingActivity() const
{
    // If the ScriptExecutionContext has been shut down on this object close()'ed, we can GC.
    if (!scriptExecutionContext() || m_isDetached)
        return false;

    // If this MessagePort has no message event handler then there is no point in keeping it alive.
    if (!m_hasMessageEventListener)
        return false;

    return m_entangled;
}

MessagePort* MessagePort::locallyEntangledPort() const
{
    // FIXME: As the header describes, this is an optional optimization.
    // Even in the new async model we should be able to get it right.
    return nullptr;
}

ExceptionOr<Vector<TransferredMessagePort>> MessagePort::disentanglePorts(Vector<Ref<MessagePort>>&& ports)
{
    if (ports.isEmpty())
        return Vector<TransferredMessagePort> { };

    // Walk the incoming array - if there are any duplicate ports, or null ports or cloned ports, throw an error (per section 8.3.3 of the HTML5 spec).
    HashSet<Ref<MessagePort>> portSet;
    for (auto& port : ports) {
        if (!port->m_entangled || !portSet.add(port).isNewEntry)
            return Exception { ExceptionCode::DataCloneError };
    }

    // Passed-in ports passed validity checks, so we can disentangle them.
    return WTF::map(ports, [](auto& port) {
        return port->disentangle();
    });
}

bool MessagePort::addEventListener(const AtomString& eventType, Ref<EventListener>&& listener, const AddEventListenerOptions& options)
{
    if (eventType == eventNames().messageEvent) {
        if (listener->isAttribute())
            start();
        m_hasMessageEventListener = true;
    }

    return EventTarget::addEventListener(eventType, WTF::move(listener), options);
}

bool MessagePort::removeEventListener(const AtomString& eventType, EventListener& listener, const EventListenerOptions& options)
{
    bool result = EventTarget::removeEventListener(eventType, listener, options);

    if (!hasEventListeners(eventNames().messageEvent))
        m_hasMessageEventListener = false;

    return result;
}

ScriptExecutionContext* MessagePort::scriptExecutionContext() const
{
    return ActiveDOMObject::scriptExecutionContext();
}

WebCoreOpaqueRoot root(MessagePort* port)
{
    return WebCoreOpaqueRoot { port };
}

} // namespace WebCore
