#pragma once
#include <stdint.h>
#include "xGuiEvent.h"

namespace idisplay {

// One push button: turns the edges its pin interrupt sees into key
// events. No RTOS and no HAL here, so it is safe in an interrupt: the
// caller reads the pin, says whether the button is down, and posts the
// event if there is one (xGuiButton in hw/freertos does that).
//
// Debouncing, for an interrupt on both edges:
//  - a press is taken only if the button has been up for debounceMs,
//    so the bounces of a release, and of the press itself, are not
//    seen as presses;
//  - a release is reported (if asked for) only once the press it ends
//    has lasted debounceMs. A release inside that is taken as bounce
//    (a tap shorter than debounceMs is a press with no release).
// The GUI's one-slot event queue does the rest: while a key is waiting
// to be handled, further presses are dropped rather than queued up.
class xButton {
public:
    explicit xButton(xKey key, uint16_t debounceMs = 50, bool reportReleases = false)
        : key_(key), debounceMs_(debounceMs), reportReleases_(reportReleases) {}

    xKey key() const { return key_; }
    bool isDown() const { return down_; }

    // Call on every edge (or every poll) with the button's state now.
    // True when ev holds an event to post.
    bool onLevel(bool down, uint32_t nowMs, xGuiEvent& ev);

private:
    xKey key_;
    uint16_t debounceMs_;
    bool reportReleases_;
    volatile bool down_ = false;
    volatile bool pressActive_ = false;      // a press was reported and not yet released
    volatile bool everReleased_ = false;     // releasedAt_ is meaningful
    volatile uint32_t releasedAt_ = 0;
    volatile uint32_t pressedAt_ = 0;
};

} // namespace idisplay
