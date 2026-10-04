#pragma once
#include "Safe.h"
#include "SafeInterlock.h"
#include "EventQueue.h"

// Bridges a Safe (e.g. a SafeDevice representing an internal, CAN-
// based safety relay on a drone — a motor controller health check, a
// redundant IMU pair) to an EventQueue — whenever the watched Safe's
// evaluated state changes, pushes a corresponding Event. This
// connects the Safe/SafeDevice hierarchy (built for the electrical-
// relay, must-act-now case: manual-reset latch, discrepancy-means-
// unsafe, never optimistically safe) to the Event/EventQueue
// hierarchy (built for the graduated-warning, notify-the-swarm case)
// without either hierarchy needing to know the other exists.
//
// Always evaluates through SafeInterlock, never a raw GetSafe1State()/
// GetSafe2State() read — same discipline as every other Safe consumer
// in this codebase.
//
// Change-gated: only pushes an event when the evaluated state
// actually transitions, not on every callback fire — same convention
// as SafeDevice's own output-write gating, avoiding a flood of
// redundant events for a Safe that's merely being re-confirmed.
//
// Does NOT set Event::timestamp — this class has no assumption about
// which clock a given platform uses (HAL_GetTick() on bare-metal
// STM32, osKernelGetTickCount() under FreeRTOS, a different clock
// entirely on a Linux-based drone controller). It's left at Event's
// own default (0); a caller that needs a real timestamp should set
// one via a subclass or a wrapping mechanism appropriate to their
// own platform, not something this generic bridge should guess at.
class SafeToEventBridge {
public:
    // safe/queue must both outlive this object. safeCode/unsafeCode
    // are the EventCodes pushed when the watched Safe transitions to
    // safe/unsafe respectively — caller-supplied, since what a given
    // transition actually MEANS is entirely domain-specific (e.g.
    // a drone-specific "MotorControllerFaulted" code).
    SafeToEventBridge(Safe& safe, EventQueue& queue, EventCode safeCode, EventCode unsafeCode)
        : safe_(safe), queue_(queue), safeCode_(safeCode), unsafeCode_(unsafeCode) {
        safe_.SetSafe1Callback(&SafeToEventBridge::onSafeChanged, this);
        safe_.SetSafe2Callback(&SafeToEventBridge::onSafeChanged, this);
        handleSafeChanged(); // establish initial state and push an initial event
    }

private:
    static void onSafeChanged(Safe& /*source*/, void* context) {
        static_cast<SafeToEventBridge*>(context)->handleSafeChanged();
    }

    void handleSafeChanged() {
        const bool nowSafe = SafeInterlock::isFullySafe(safe_);
        if (everReported_ && nowSafe == lastReportedSafe_) {
            return; // no actual change — don't push a redundant event
        }
        lastReportedSafe_ = nowSafe;
        everReported_ = true;

        Event event;
        event.code = nowSafe ? safeCode_ : unsafeCode_;
        queue_.pushEvent(event);
    }

    Safe&       safe_;
    EventQueue& queue_;
    EventCode   safeCode_;
    EventCode   unsafeCode_;
    bool        lastReportedSafe_ = false;
    bool        everReported_ = false;
};
