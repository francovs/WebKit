/*
 * Copyright (C) 2025 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "EventCounts.h"

#include "EventNames.h"
#include "bindings/IDLTypes.h"
#include <bindings/js/JSDOMMapLike.h>
#include "wtf/Assertions.h"
#include "wtf/HashSet.h"
#include <initializer_list>


namespace WebCore {

EventCounts::EventCounts()
: m_counts{
    {eventNames().auxclickEvent              , 0},
    {eventNames().clickEvent                 , 0},
    {eventNames().contextmenuEvent           , 0},
    {eventNames().dblclickEvent              , 0},
    {eventNames().mousedownEvent             , 0},
    {eventNames().mouseenterEvent            , 0},
    {eventNames().mouseleaveEvent            , 0},
    {eventNames().mouseoutEvent              , 0},
    {eventNames().mouseoverEvent             , 0},
    {eventNames().mouseupEvent               , 0}, 
    {eventNames().pointeroverEvent          , 0},
    {eventNames().pointerenterEvent         , 0},
    {eventNames().pointerdownEvent          , 0},
    {eventNames().pointerupEvent            , 0},
    {eventNames().pointercancelEvent        , 0},
    {eventNames().pointeroutEvent           , 0},
    {eventNames().pointerleaveEvent         , 0},
    {eventNames().gotpointercaptureEvent    , 0},
    {eventNames().lostpointercaptureEvent   , 0},
    {eventNames().touchstartEvent           , 0},
    {eventNames().touchendEvent             , 0},
    {eventNames().touchcancelEvent          , 0},
    {eventNames().keydownEvent              , 0},
    {eventNames().keypressEvent             , 0},
    {eventNames().keyupEvent                , 0},
    {eventNames().beforeinputEvent          , 0},
    {eventNames().inputEvent                , 0},
    {eventNames().compositionstartEvent     , 0},
    {eventNames().compositionupdateEvent    , 0},
    {eventNames().compositionendEvent       , 0},
    {eventNames().dragstartEvent            , 0},
    {eventNames().dragendEvent              , 0},
    {eventNames().dragenterEvent            , 0},
    {eventNames().dragleaveEvent            , 0},
    {eventNames().dragoverEvent             , 0},
    {eventNames().dropEvent                 , 0}
} { }


EventCounts::~EventCounts() { }

void EventCounts::initializeMapLike(DOMMapAdapter& map)
{
    ALWAYS_LOG_WITH_STREAM(stream << "Initializing EventCount maplike");
    // TODO: our maplike implementation causes a new JS object to be created
    // the first time the EventCounts object is accessed through javascript.
    // This new object has its own, separate key-value storage. This is bad
    // because it requires incrementing both m_counts and the JS-accessible
    // m_maplike independently.
    //
    // Ideally, maplike would simply proxy reads to m_counts instead.
    ASSERT(!m_maplike);
    m_maplike = std::make_unique<DOMMapAdapter>(map);
    for (auto& kv : m_counts) {
        map.set<IDLDOMString, IDLUnsignedLongLong>(String(kv.key), kv.value);
    }
}

void EventCounts::inc(const AtomString &eventType)
{
    ASSERT(m_counts.contains(eventType));
    unsigned newCount = m_counts.get(eventType) + 1;
    m_counts.set(eventType, newCount);
    /*
    // I am not 100% sure the DOMMapAdapter::m_backingMap object's lifetime
    // is tied to the EventCounts object that caused it to be initialized. If it
    // isn't, using m_maplike as follows could cause use-after-free:
    //
    // Update: this absolutely corrupts memory, probably because the JSObject
    // gets relocated and m_backingMap becomes stale?
    if (m_maplike) {
        m_maplike->set<IDLDOMString, IDLUnsignedLongLong>(String(eventType), newCount);
    }
    */
}

bool EventCounts::IsCandidateForEventTiming(const AtomString &eventType)
{
    static NeverDestroyed<HashSet<AtomString>> countedEvents(std::initializer_list<AtomString>{
        eventNames().auxclickEvent,
        eventNames().clickEvent,
        eventNames().contextmenuEvent,
        eventNames().dblclickEvent,
        eventNames().mousedownEvent,
        eventNames().mouseenterEvent,
        eventNames().mouseleaveEvent,
        eventNames().mouseoutEvent,
        eventNames().mouseoverEvent,
        eventNames().mouseupEvent, 
        eventNames().pointeroverEvent,
        eventNames().pointerenterEvent,
        eventNames().pointerdownEvent,
        eventNames().pointerupEvent,
        eventNames().pointercancelEvent,
        eventNames().pointeroutEvent,
        eventNames().pointerleaveEvent,
        eventNames().gotpointercaptureEvent,
        eventNames().lostpointercaptureEvent,
        eventNames().touchstartEvent,
        eventNames().touchendEvent,
        eventNames().touchcancelEvent,
        eventNames().keydownEvent,
        eventNames().keypressEvent,
        eventNames().keyupEvent,
        eventNames().beforeinputEvent,
        eventNames().inputEvent,
        eventNames().compositionstartEvent,
        eventNames().compositionupdateEvent,
        eventNames().compositionendEvent,
        eventNames().dragstartEvent,
        eventNames().dragendEvent,
        eventNames().dragenterEvent,
        eventNames().dragleaveEvent,
        eventNames().dragoverEvent,
        eventNames().dropEvent
    });
    return countedEvents->contains(eventType);
}

} // namespace WebCore
