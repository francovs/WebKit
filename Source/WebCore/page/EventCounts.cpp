/*
 * Copyright (C) 2025 Apple Inc. All rights reserved.
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
#include "EventCounts.h"

#include "IDLTypes.h"
#include "JSDOMMapLike.h"
#include "bindings/js/WebCoreJSClientData.h"
#include "wtf/StdLibExtras.h"
#include <algorithm>

namespace WebCore {

WTF_MAKE_TZONE_OR_ISO_ALLOCATED_IMPL(EventCounts);

EventCounts::EventCounts()
{ }

void EventCounts::add(EventType type)
{
    size_t index = std::ranges::lower_bound(EventNames::TimedEvents, type) - EventNames::TimedEvents.begin();
    ASSERT(index < m_counts.size());
    ++m_counts[index];

    auto wrapperObject = wrapper();
    if (wrapperObject) {
        auto& vm = wrapperObject->vm();
        auto backingMap = wrapperObject->getDirect(vm, builtinNames(vm).backingMapPrivateName());
        ASSERT(backingMap);
        auto mapAdapter = DOMMapAdapter(*wrapperObject->globalObject(), *JSC::asObject(backingMap));
        if (!allEventNames)
            allEventNames = WTF::makeUnique<Vector<const AtomString>>(eventNames().allEventNames());
        size_t typeAsIndex = static_cast<size_t>(EventNames::TimedEvents[index]) - 1;
        mapAdapter.set<IDLDOMString, IDLUnsignedLongLong>((*allEventNames)[typeAsIndex], m_counts[index]);
    }
}

void EventCounts::initializeMapLike(DOMMapAdapter& map)
{
    if (!allEventNames)
        allEventNames = WTF::makeUnique<Vector<const AtomString>>(eventNames().allEventNames());

    for (size_t index = 0; index < EventNames::TimedEvents.size(); ++index) {
        // Subtract 1 to account for EventType::custom
        size_t typeAsIndex = static_cast<size_t>(EventNames::TimedEvents[index]) - 1;
        ASSERT(typeAsIndex < allEventNames->size());
        map.set<IDLDOMString, IDLUnsignedLongLong>((*allEventNames)[typeAsIndex], m_counts[index]);
    }
}

} // namespace WebCore
