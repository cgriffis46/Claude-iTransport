#include "SafeDevice.h"
#include "SafeInterlock.h"

SafeDevice::SafeDevice(SafeInput& input1, SafeInput& input2, SafeOutput& output, SafeInput* resetInput)
    : input1_(input1), input2_(input2), output_(output), resetInput_(resetInput) {
    // Registered on BOTH Safe1 and Safe2 of each input, not just one:
    // SafeInput's own single-channel convention means Safe1==Safe2
    // for the concrete inputs built so far, but SafeDevice doesn't
    // assume that — any change on either channel of either input
    // should be noticed.
    input1_.SetSafe1Callback(&SafeDevice::onInputChanged, this);
    input1_.SetSafe2Callback(&SafeDevice::onInputChanged, this);
    input2_.SetSafe1Callback(&SafeDevice::onInputChanged, this);
    input2_.SetSafe2Callback(&SafeDevice::onInputChanged, this);

    if (resetInput_) {
        resetInput_->SetSafe1Callback(&SafeDevice::onResetInputChanged, this);
        resetInput_->SetSafe2Callback(&SafeDevice::onResetInputChanged, this);
    }

    // latchedUnsafe_ already starts true (boot-up assumption) — this
    // just establishes the resulting reported state and drives the
    // output to match, rather than leaving it undriven. Note this
    // does NOT consult resetInput_'s startup value: even if a reset
    // input happens to already read true at construction time (a
    // button that was already being held down, say), the latch
    // still starts true here — it's only handleResetInputChanged()
    // noticing a CHANGE afterward that triggers reset(), matching
    // the same "the callback fires on change" behavior every other
    // SafeInput/SafeOutput in this codebase already has.
    updateReportedState();
}

void SafeDevice::reset() {
    // Only clears the latch if the inputs currently agree safe RIGHT
    // NOW — reset() never forces safe unconditionally. If they don't
    // agree, latchedUnsafe_ simply stays true (it was already true),
    // and updateReportedState() below correctly changes nothing.
    if (rawInputsSafe()) {
        latchedUnsafe_ = false;
    }
    updateReportedState();
}

void SafeDevice::onInputChanged(Safe& /*source*/, void* context) {
    static_cast<SafeDevice*>(context)->handleInputChanged();
}

void SafeDevice::onResetInputChanged(Safe& /*source*/, void* context) {
    static_cast<SafeDevice*>(context)->handleResetInputChanged();
}

void SafeDevice::handleResetInputChanged() {
    // reset()'s own idempotency (see its comment) makes this safe to
    // call every time resetInput_ reports true, without needing
    // separate rising-edge detection here: SafeInput's own change-
    // detection already means this only fires once per false->true
    // transition (holding the button down doesn't refire it), and
    // reset() itself does nothing if the latch was already clear.
    if (resetInput_ && resetInput_->GetSafe1State()) {
        reset();
    }
}

bool SafeDevice::rawInputsSafe() const {
    // Deliberately a plain AND, not SafeInterlock: any disagreement
    // between input1_ and input2_ (one true, one false) already
    // evaluates to false here, which is exactly the "must latch"
    // outcome — no need for SafeInterlock's Discrepancy-vs-Unsafe
    // distinction at this level, since both non-agreement cases are
    // treated identically by the latch.
    return input1_.GetSafe1State() && input2_.GetSafe1State();
}

void SafeDevice::handleInputChanged() {
    // Going unsafe is NEVER gated: if the raw inputs currently
    // disagree, or both say unsafe, latch immediately — even if
    // we're already past a previous reset() and currently reporting
    // safe. Only the RETURN to safe requires the explicit reset()
    // call in reset() above; nothing here ever clears the latch.
    if (!rawInputsSafe()) {
        latchedUnsafe_ = true;
    }
    updateReportedState();
}

void SafeDevice::updateReportedState() {
    // While latched, Safe1/Safe2 report false regardless of what the
    // raw inputs actually say — anything consuming SafeDevice as a
    // Safe& (including output_'s own decision, via SafeInterlock
    // below) must see the latch-aware decision, not a misleadingly
    // "healthy" raw channel value.
    const bool reportedSafe1 = latchedUnsafe_ ? false : input1_.GetSafe1State();
    const bool reportedSafe2 = latchedUnsafe_ ? false : input2_.GetSafe1State();

    reportState(safe1_, safe1Callback_, safe1CallbackContext_, reportedSafe1);
    reportState(safe2_, safe2Callback_, safe2CallbackContext_, reportedSafe2);

    // Drive the output based on the COMBINED, discrepancy-aware,
    // latch-aware decision — never based on either channel alone.
    // Change-gated: only actually writes when the computed decision
    // differs from what was last driven, except the very first call
    // (from the constructor), which always writes so the physical
    // output ends up in a known state rather than whatever the
    // hardware happened to power on in.
    const bool newOutputState = SafeInterlock::isFullySafe(*this);
    if (!outputInitialized_ || newOutputState != lastOutputState_) {
        output_.setDesiredState(newOutputState);
        lastOutputState_   = newOutputState;
        outputInitialized_ = true;
    }
}

void SafeDevice::reportState(bool& stateField, SafeCallback callback, void* context, bool newState) {
    const bool changed = (stateField != newState);
    stateField = newState;
    if (changed && callback) callback(*this, context);
}
