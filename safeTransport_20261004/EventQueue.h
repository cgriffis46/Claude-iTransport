#pragma once
#include "Event.h"

// Abstract interface for a queue of Events, with callback-based
// notification — deliberately NOT tied to any specific RTOS or
// platform primitive, same "generic interface, platform-specific
// implementation" pattern as iTransport/SafeInput/SafeOutput
// elsewhere in this codebase. Drone firmware and central server
// software both need this same CONTRACT but genuinely different
// underlying mechanisms (a FreeRTOS queue on the drone, something
// POSIX-appropriate on the server) — this interface lets event-
// producing and event-consuming code be written once, against this
// contract, regardless of which concrete queue backs it (the same
// way SimpleSafeInput worked unmodified across CAN and Ethernet
// transports built on the same iTransport interface).
class EventQueue {
public:
    virtual ~EventQueue() = default;

    using EventCallback = void (*)(const Event& event, void* context);

    // Pushes a new event onto the queue. Returns false if the event
    // could not be accepted (e.g. the queue is full) — a concrete
    // implementation must never silently drop a safety-relevant
    // event without the caller being able to find out; see the
    // concrete class actually used for its own full guarantees, and
    // whether it's safe to call from ISR context.
    virtual bool pushEvent(const Event& event) = 0;

    // Registers a callback invoked once per event, in the order
    // events were pushed, as they're actually processed (via
    // processEvents() below) — NOT at push time. Matches this
    // codebase's "cooperative, explicit processing" convention
    // (SafeZone::EvaluateSafe(), UartLoopbackChannelSafeInput::poll())
    // rather than an immediate callback that might fire from
    // whatever context pushEvent() was called from.
    virtual void setEventCallback(EventCallback callback, void* context = nullptr) = 0;

    // Processes whatever events are CURRENTLY pending, invoking the
    // registered callback once per event. Call this periodically
    // (e.g. from a drone's own main loop, or a server's own event-
    // processing thread).
    virtual void processEvents() = 0;
};
