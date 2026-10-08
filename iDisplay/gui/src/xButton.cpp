#include "xButton.h"

namespace idisplay {

bool xButton::onLevel(bool down, uint32_t nowMs, xGuiEvent& ev) {
    if (down == down_) return false;     // no edge: a bounce the pin settled from
    down_ = down;

    if (down) {
        // Unsigned subtraction keeps this right across the tick rollover.
        const bool settled = !everReleased_ || (uint32_t)(nowMs - releasedAt_) >= debounceMs_;
        if (!settled) return false;
        pressActive_ = true;
        pressedAt_ = nowMs;
        ev = xGuiEvent::keyEvent(key_, xKeyAction::Pressed);
        return true;
    }

    releasedAt_ = nowMs;
    everReleased_ = true;
    // A release inside debounceMs of the press is its bounce. Should it
    // have been a real tap that short, the next press is still taken
    // (the button has been up long enough); only this release goes
    // unreported.
    if (!pressActive_ || (uint32_t)(nowMs - pressedAt_) < debounceMs_) return false;
    pressActive_ = false;
    if (!reportReleases_) return false;
    ev = xGuiEvent::keyEvent(key_, xKeyAction::Released);
    return true;
}

} // namespace idisplay
