#pragma once
#include <cstddef>
#include "EventQueue.h"

// A fixed-size, ring-buffer-based EventQueue — no dynamic allocation,
// no RTOS-specific primitives, works identically on any platform.
// This is the reference/default implementation, proving the
// EventQueue abstraction actually works; FreeRTOS-specific (a native
// queue, matching xBNO085's own frameQueue_ pattern) and Linux-
// specific (a mutex-protected structure) concrete implementations
// are natural next steps if a specific platform's concurrency needs
// require them.
//
// NOT thread-safe: pushEvent()/processEvents() must be called from
// the SAME context, or externally synchronized by the caller — there
// is no internal locking here. Fine for a single-threaded producer
// and consumer, or where the caller wraps calls in their own mutex/
// critical section; NOT safe to call pushEvent() from an ISR while
// processEvents() runs on a task without additional synchronization
// the caller provides on top of this class.
template <size_t Capacity>
class SimpleEventQueue : public EventQueue {
public:
    bool pushEvent(const Event& event) override {
        if (count_ >= Capacity) {
            return false; // full — never silently overwrite or drop a pending event
        }
        events_[(head_ + count_) % Capacity] = event;
        ++count_;
        return true;
    }

    void setEventCallback(EventCallback callback, void* context = nullptr) override {
        callback_ = callback;
        callbackContext_ = context;
    }

    void processEvents() override {
        while (count_ > 0) {
            const Event& event = events_[head_];
            if (callback_) {
                callback_(event, callbackContext_);
            }
            head_ = (head_ + 1) % Capacity;
            --count_;
        }
    }

    // Exposed for testing/diagnostics — how many events are currently pending.
    size_t pendingCount() const { return count_; }

private:
    Event  events_[Capacity];
    size_t head_  = 0;
    size_t count_ = 0;
    EventCallback callback_ = nullptr;
    void*         callbackContext_ = nullptr;
};
