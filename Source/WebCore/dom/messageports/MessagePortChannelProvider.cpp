/*
 * Copyright (C) 2018 Apple Inc. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "MessagePortChannelProvider.h"

#include "Document.h"
#include "MessagePort.h"
#include "WorkerGlobalScope.h"
#include "WorkletGlobalScope.h"
#include <wtf/MainThread.h>
#include <wtf/NeverDestroyed.h>

namespace WebCore {

WTF_MAKE_TZONE_ALLOCATED_IMPL(MessagePortChannelProvider::PortEntry);

/*
Replacements:
 - createNewMessagePortChannel -> registerNewChannel
 - entangleLocalPortInThisProcessToRemote -> claimPort
 - hideous MessagePort lambda -> scheduleHandlingForPort
 - messagePortDisentangled -> disentangleForShipping
 - takeAllMessagesForPort -> takeOneMessage + handleAllQueuedMessages
 - postMessageToRemote -> postMessageFromPort
 -  *nothing* -> startPort()
 - messagePortClosed -> closePort()
 - *nothing* -> messagePortRemoved (for collection without explicit .close() - see https://github.com/whatwg/html/issues/10201)


IPC Replacements:
 - Messages::NetworkConnectionToWebProcess::PostMessageToRemote 
 - Messages::NetworkConnectionToWebProcess::DidDeliverMessagePortMessages
 - Messages::NetworkConnectionToWebProcess::DropNonSerializableInProcessCache
 - Messages::NetworkConnectionToWebProcess::MessagePortClosed
 - Messages::NetworkConnectionToWebProcess::MessagePortDisentangled
 - Messages::NetworkConnectionToWebProcess::EntangleLocalPortInThisProcessToRemote
 - Messages::NetworkConnectionToWebProcess::CreateNewMessagePortChannel
*/


//
// Initialization logic:
//

static RefPtr<MessagePortChannelProvider>& NODELETE globalProvider()
{
    static NeverDestroyed<RefPtr<MessagePortChannelProvider>> globalProvider;
    return globalProvider;
}

MessagePortChannelProvider& MessagePortChannelProvider::singleton()
{
    //ASSERT(isMainThread());
    auto& globalProvider = WebCore::globalProvider();
    if (!globalProvider)
        globalProvider = MessagePortChannelProvider::create(nullptr);
    return *globalProvider;
}

void MessagePortChannelProvider::setSharedProvider(Ref<RemoteProvider>&& provider)
{
    RELEASE_ASSERT(isMainThread());
    auto& globalProvider = WebCore::globalProvider();
    RELEASE_ASSERT(!globalProvider);
    globalProvider = MessagePortChannelProvider::create(WTF::move(provider));
}

MessagePortChannelProvider::MessagePortChannelProvider(RefPtr<RemoteProvider>&& provider)
: m_remoteProvider{WTF::move(provider)} 
{ }

IGNORE_WARNINGS_BEGIN("missing-noreturn")
MessagePortChannelProvider::~MessagePortChannelProvider()
{
    ASSERT_NOT_REACHED();
}
IGNORE_WARNINGS_END

Ref<MessagePortChannelProvider> MessagePortChannelProvider::create(RefPtr<RemoteProvider>&& provider)
{
    return adoptRef(*new MessagePortChannelProvider(WTF::move(provider)));
}

//
// Same-process logic:
//

bool MessagePortChannelProvider::registerNewChannel(ScriptExecutionContext& context, Ref<MessagePort> port1, Ref<MessagePort> port2)
{
    ASSERT(context.isContextThread());
    Locker lock { m_portLock };
    if (port1->isDetached() || port2->isDetached())
        return false;

    auto port1Entry = makeUniqueRef<PortEntry>(port1, port2->identifier());
    m_portsInProcess.add(port1->identifier(), WTF::move(port1Entry));

    auto port2Entry = makeUniqueRef<PortEntry>(port2, port1->identifier());
    m_portsInProcess.add(port2->identifier(), WTF::move(port2Entry));

    port1->didRegister();
    port2->didRegister();
    return true;
}

void MessagePortChannelProvider::postMessageFromPort(MessageWithMessagePorts&& message, MessagePortIdentifier senderIdentifier)
{
    Locker lock { m_portLock };

    auto senderEntry = m_portsInProcess.find(senderIdentifier);
    if (senderEntry == m_portsInProcess.end()) {
        RELEASE_ASSERT_NOT_REACHED();
    }

    auto targetId = senderEntry->value->remoteEnd;
    auto targetEntry = m_portsInProcess.find(targetId);
    if (targetEntry == m_portsInProcess.end()) {
        RELEASE_ASSERT_NOT_REACHED();
        postMessageToRemote(targetId, WTF::move(message));
        return;
    }

    targetEntry->value->messageQueue.append(WTF::move(message));
    if (!targetEntry->value->isStarted)
        return;

    if (RefPtr<MessagePort> targetPort = targetEntry->value->entangledTo.get())
        targetPort.releaseNonNull()->scheduleHandlingForMessages(1);
}

MessageWithMessagePorts MessagePortChannelProvider::takeOneMessage(MessagePortIdentifier portId)
{
    Locker lock { m_portLock };
    auto it = m_portsInProcess.find(portId);
    ASSERT(it != m_portsInProcess.end());
    ASSERT(it->value->messageQueue.size());
    return it->value->messageQueue.takeFirst();
}

Vector<Ref<MessagePort>> MessagePortChannelProvider::claimShippedPorts(ScriptExecutionContext& context, Vector<TransferredMessagePort> ports)
{
    Locker lock { m_portLock };
    Vector<Ref<MessagePort>> claimedPorts;
    for (auto port : ports) {
        auto it = m_portsInProcess.find(port.first);
        // FIXME: claim ports from remote as well
        ASSERT(!it->value->entangledTo.get());
        auto newPort = MessagePort::create(context, port.first, port.second);
        it->value->entangledTo = ThreadSafeWeakPtr<MessagePort> { newPort };
        newPort->didRegister();
        claimedPorts.append(WTF::move(newPort));
    }
    return claimedPorts;
}

void MessagePortChannelProvider::startPort(Ref<MessagePort> port)
{
    Locker lock { m_portLock };
    auto it = m_portsInProcess.find(port->identifier());
    ASSERT(it != m_portsInProcess.end());
    ASSERT(!it->value->isStarted);
    it->value->isStarted = true;
    port->scheduleHandlingForMessages(it->value->messageQueue.size());
}

void MessagePortChannelProvider::closePort(MessagePortIdentifier id)
{
    // FIXME: implement a proper closing protocol
    Locker lock { m_portLock };
    auto it = m_portsInProcess.find(id);
    ASSERT(it != m_portsInProcess.end());
    it->value->entangledTo = nullptr;
    if (it->value->remoteClosed)
        m_portsInProcess.remove(it);
    else {
        auto it2 = m_portsInProcess.find(it->value->remoteEnd);
        // FIXME: tell the remote end we've closed
        if (it2 != m_portsInProcess.end())
            it2->value->remoteClosed = true;
    }
}

void MessagePortChannelProvider::deactivatePort(MessagePortIdentifier id)
{
    // FIXME: deactivatePort should 
    closePort(id);
}

void MessagePortChannelProvider::disentangleForShipping(MessagePortIdentifier identifier)
{
    Locker lock { m_portLock };
    UNUSED_PARAM(identifier);
    auto portEntry = m_portsInProcess.find(identifier);
    RELEASE_ASSERT(portEntry != m_portsInProcess.end());
    portEntry->value->entangledTo = nullptr;
    portEntry->value->isStarted = false;
}

//
// Cross process stuff:
//

void MessagePortChannelProvider::postMessageToRemote(MessagePortIdentifier target, MessageWithMessagePorts&& message)
{
    UNUSED_PARAM(target);
    UNUSED_PARAM(message);

    Vector<std::pair<MessagePortIdentifier, bool>> remoteRegistryUpdates;
    for (auto port : message.transferredPorts)
        remoteRegistryUpdates.appendVector(collectRemotePortRegistryUpdates(port.first));

    //RemoteProvider::singleton().postMessageToRemote(target, message, WTF::move(remoteRegistryUpdates));
}

void MessagePortChannelProvider::gotMessagesForPort(Vector<MessageWithMessagePorts>&& messages, MessagePortIdentifier destination)
{
    Locker lock { m_portLock };

    auto targetEntry = m_portsInProcess.find(destination);
    if (targetEntry == m_portsInProcess.end()) {
        // FIXME: implement rerouting hot-potatoed ports through the NetworkProcess
        return;
    }

    size_t messagesToHandle = messages.size();
    for (auto& message : messages)
        targetEntry->value->messageQueue.append(WTF::move(message));

    if (!targetEntry->value->isStarted)
        return;

    if (RefPtr<MessagePort> targetPort = targetEntry->value->entangledTo.get())
        targetPort.releaseNonNull()->scheduleHandlingForMessages(messagesToHandle);
}

Vector<std::pair<MessagePortIdentifier, bool>> MessagePortChannelProvider::collectRemotePortRegistryUpdates(MessagePortIdentifier portBeingSent)
{
    UNUSED_PARAM(portBeingSent);
    auto it = m_portsInProcess.find(portBeingSent);
    ASSERT(it != m_portsInProcess.end());
    ASSERT(!it->value->entangledTo.get());

    // bool represents whether the port is in the current process or not
    Vector<std::pair<MessagePortIdentifier, bool>> collectedPortUpdates;
    collectedPortUpdates.append({portBeingSent, false});

    auto remoteEndIterator = m_portsInProcess.find(it->value->remoteEnd);
    if (remoteEndIterator != m_portsInProcess.end())
        collectedPortUpdates.append({portBeingSent, true});

    // FIXME: we also need to recursively send all ports inside it->value->messageQueue
    return collectedPortUpdates;
}


//
// What is this?
//

IGNORE_WARNINGS_BEGIN("missing-noreturn")
void MessagePortChannelProvider::dropNonSerializableInProcessCache(NonSerializedDataIdentifier id)
{
    UNUSED_PARAM(id);
    RELEASE_ASSERT_NOT_REACHED();
}
IGNORE_WARNINGS_END

} // namespace WebCore
