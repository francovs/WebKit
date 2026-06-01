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

#pragma once

#include <WebCore/ProcessIdentifier.h>
#include <WebCore/NonSerializedDataIdentifier.h>
#include "MessagePortIdentifier.h"
#include "MessageWithMessagePorts.h"
#include <wtf/AbstractRefCounted.h>
#include <wtf/CanMakeWeakPtr.h>
#include <wtf/CompletionHandler.h>
#include <wtf/HashMap.h>
#include <wtf/Lock.h>
#include <wtf/RefCounted.h>
#include <wtf/UniqueRef.h>
#include <wtf/Vector.h>

namespace WebCore {

class ScriptExecutionContext;
struct MessagePortIdentifier;
struct MessageWithMessagePorts;


class RemoteProvider: public AbstractRefCounted {
public:
    virtual void postMessageToRemote(Vector<std::pair<MessagePortIdentifier, bool>> updates) = 0;
};

class MessagePortChannelProvider final: public CanMakeWeakPtr<MessagePortChannelProvider>, public RefCounted<MessagePortChannelProvider> {
public:
    WEBCORE_EXPORT static MessagePortChannelProvider& singleton();
    WEBCORE_EXPORT static void setSharedProvider(Ref<RemoteProvider>&&);

    ~MessagePortChannelProvider();
    void ref() const { RefCounted::ref(); }
    void deref() const { RefCounted::deref(); }

private:
    MessagePortChannelProvider(RefPtr<RemoteProvider>&&);
    static Ref<MessagePortChannelProvider> create(RefPtr<RemoteProvider>&&);
    RefPtr<RemoteProvider> m_remoteProvider;

public:
    // FIXME: what does this do? Still need to plug it in:
    WEBCORE_EXPORT void dropNonSerializableInProcessCache(NonSerializedDataIdentifier);

    // New interface:
    bool registerNewChannel(ScriptExecutionContext&, Ref<MessagePort>, Ref<MessagePort>);
    void postMessageFromPort(MessageWithMessagePorts&&, MessagePortIdentifier);
    MessageWithMessagePorts takeOneMessage(MessagePortIdentifier);
    Vector<Ref<MessagePort>> claimShippedPorts(ScriptExecutionContext&, Vector<TransferredMessagePort>);
    void startPort(Ref<MessagePort>);
    void closePort(MessagePortIdentifier);
    void deactivatePort(MessagePortIdentifier);
    void disentangleForShipping(MessagePortIdentifier);

    // IPC:
    void gotMessagesForPort(Vector<MessageWithMessagePorts>&&, MessagePortIdentifier);

private:
    class PortEntry {
        WTF_MAKE_TZONE_ALLOCATED(PortEntry);
    public:
        PortEntry(ThreadSafeWeakPtr<MessagePort> entangledTo, MessagePortIdentifier remoteEnd)
        : entangledTo{WTF::move(entangledTo)}
        , remoteEnd{WTF::move(remoteEnd)}
        { }

        ThreadSafeWeakPtr<MessagePort> entangledTo;
        MessagePortIdentifier remoteEnd;
        // FIXME: consider lock free alternative:
        Deque<MessageWithMessagePorts> messageQueue;
        bool isStarted { false };
        bool remoteClosed { false };
    };

    HashMap<MessagePortIdentifier, UniqueRef<PortEntry>> m_portsInProcess;
    Lock m_portLock;

    // IPC:
    Vector<std::pair<MessagePortIdentifier, bool>> collectRemotePortRegistryUpdates(MessagePortIdentifier);
    void postMessageToRemote(MessagePortIdentifier target, MessageWithMessagePorts&& message);
};

} // namespace WebCore
