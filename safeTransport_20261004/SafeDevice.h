#pragma once
#include "Safe.h"
#include "SafeInput.h"
#include "SafeOutput.h"

// A complete "read two redundant channels, decide correctly, act"
// unit — exactly the two-relay / two-e-stop-contact redundancy
// pattern: input1 is one of the two independent channels, input2 is
// the other (SafeDevice's own Safe1 comes from input1, Safe2 from
// input2), with output automatically driven by the COMBINED,
// discrepancy-aware decision (via SafeInterlock, in the .cpp) rather
// than either channel being trusted alone.
//
// MANUAL RESET LATCH: on construction (which, for an embedded system,
// IS what "boots up after a power failure" looks like — the
// constructor runs again from scratch either way), this starts
// latched unsafe, regardless of what input1_/input2_ happen to
// report. It stays latched — reporting unsafe on Safe1/Safe2 and
// keeping the output de-energized — until reset() is called AND the
// inputs currently agree safe at that moment. This is the standard
// "manual reset required" behavior real safety relays have (and part
// of why IEC 60204-1-style machine safety standards require it): a
// safety fault clearing on its own must never silently re-energize
// an output without something deliberately confirming it first.
//
// The asymmetry is deliberate and important: going UNSAFE is never
// gated or delayed — if the raw inputs ever disagree, or both agree
// unsafe, this latches immediately and automatically, even while
// already past a previous reset(). Only the return to SAFE requires
// the explicit reset() call. Never optimistically safe, in other
// words, exactly like every other Safe implementation in this
// codebase — just enforced as a LATCH here rather than a one-shot
// default.
//
// Extends Safe directly, not SafeInput/SafeOutput — this class
// doesn't itself read bytes or drive hardware; it composes/aggregates
// two ALREADY-EXISTING SafeInputs and one SafeOutput, each of which
// could be backed by anything (a UART loopback, a GPIO pin, a CAN
// message, an MQTT subscription, a PLC tag) without SafeDevice
// knowing or caring which.
class SafeDevice : public Safe {
public:
    // input1/input2/output must all outlive this object, as must
    // resetInput if provided. input1 and input2 must represent the
    // TWO REDUNDANT channels of the same underlying safety decision
    // (a second relay, a second set of e-stop contacts) — not two
    // arbitrary, unrelated signals; that relationship is what makes
    // running them through SafeInterlock meaningful.
    //
    // resetInput is OPTIONAL (nullptr by default): with no reset
    // input, this behaves exactly as before — reset() must be called
    // explicitly by application code. If provided, SafeDevice
    // automatically calls reset() itself whenever resetInput reports
    // true — e.g. a GpioSafeInput wired to a physical reset button,
    // so a human pressing that button is what triggers reset(),
    // without any application code needing to poll a separate GPIO
    // itself. Deliberately a single, non-redundant input, unlike
    // input1/input2: a reset button failing just means the system
    // stays safely latched (the safe failure mode either way), not
    // the false-safe risk a failed e-stop channel would be — the
    // redundancy that matters is on input1/input2, not this.
    SafeDevice(SafeInput& input1, SafeInput& input2, SafeOutput& output,
               SafeInput* resetInput = nullptr);

    // Attempts to clear the latch: re-evaluates input1_/input2_'s
    // CURRENT state, and un-latches (allowing Safe1/Safe2 to report
    // safe and the output to re-energize) ONLY if they currently
    // agree safe. If they don't, this correctly leaves the device
    // latched unsafe — reset() never forces safe unconditionally.
    // Idempotent otherwise: calling this again with nothing actually
    // changed doesn't cause a spurious callback fire or a redundant
    // output write. Called automatically whenever resetInput reports
    // true, if one was provided — but still public and callable
    // explicitly regardless, e.g. for a resetInput-less SafeDevice,
    // or for application code that wants to trigger it directly.
    void reset();

    bool GetSafe1State() const override { return safe1_; }
    bool GetSafe2State() const override { return safe2_; }
    void SetSafe1Callback(SafeCallback callback, void* context = nullptr) override {
        safe1Callback_ = callback;
        safe1CallbackContext_ = context;
    }
    void SetSafe2Callback(SafeCallback callback, void* context = nullptr) override {
        safe2Callback_ = callback;
        safe2CallbackContext_ = context;
    }

private:
    static void onInputChanged(Safe& source, void* context);
    static void onResetInputChanged(Safe& source, void* context);
    void handleInputChanged();
    void handleResetInputChanged();
    void updateReportedState();
    void reportState(bool& stateField, SafeCallback callback, void* context, bool newState);
    bool rawInputsSafe() const;

    SafeInput&  input1_;
    SafeInput&  input2_;
    SafeOutput& output_;
    SafeInput*  resetInput_; // nullptr if no auto-reset was configured

    // Starts latched — see this class's own header comment on why:
    // boot-up (== construction, for an embedded system) is treated
    // exactly like recovering from a power failure, and both must
    // assume unsafe until an explicit reset() proves otherwise.
    bool latchedUnsafe_ = true;

    bool         safe1_ = false;
    bool         safe2_ = false;
    SafeCallback safe1Callback_ = nullptr;
    SafeCallback safe2Callback_ = nullptr;
    void*        safe1CallbackContext_ = nullptr;
    void*        safe2CallbackContext_ = nullptr;

    // Tracks what was last actually driven to output_, so
    // updateReportedState() can skip a redundant write when the
    // computed decision hasn't changed.
    bool lastOutputState_   = false;
    bool outputInitialized_ = false;
};
