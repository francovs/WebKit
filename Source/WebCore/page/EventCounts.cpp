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
#include "EventNames.h"
#include "IDLTypes.h"
#include "JSDOMMapLike.h"
#include <algorithm>

namespace WebCore {

EventCounts::EventCounts()
{ }

void EventCounts::add(EventType type)
{
    size_t index = std::ranges::lower_bound(EventNames::TimedEvents, type) - EventNames::TimedEvents.begin();
    ASSERT(index < m_counts.size());
    ++m_counts[index];

    // HACK:
    if (m_maplike) {
        size_t typeAsIndex = static_cast<size_t>(type) - 1;
        m_maplike->set<IDLDOMString, IDLUnsignedLongLong>(eventNames().allEventNames()[typeAsIndex], m_counts[index]);
    }
}

void EventCounts::initializeMapLike(DOMMapAdapter& map)
{
    // FIXME: this implementation does not work as expected; the maplike object
    // is created only once, causing it to become out-of-sync with m_counts as
    // more counts are added
    auto allEventNames = eventNames().allEventNames();
    for (size_t index = 0; index < EventNames::TimedEvents.size(); ++index) {
         // Subtract 1 to account for EventType::custom
        size_t typeAsIndex = static_cast<size_t>(EventNames::TimedEvents[index]) - 1;
        ASSERT(typeAsIndex < allEventNames.size());
        map.set<IDLDOMString, IDLUnsignedLongLong>(allEventNames[typeAsIndex], m_counts[index]);
    }
    // HACK:
    m_maplike = &map;
}

} // namespace WebCore
