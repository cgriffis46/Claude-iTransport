#pragma once
#include "Safe.h"

// Abstract base for writing/transmitting a safety-relevant value
// SOMEWHERE — deliberately not committed to "there's a transport
// involved" either, same reasoning as SafeInput. A SafeOutput could
// write over a byte-stream transport (CAN, Ethernet, RS485, UART...),
// or drive a GPIO output pin directly (setting a voltage level, no
// bytes at all), or publish an MQTT message, or write a PLC tag.
//
// send() is the one thing every concrete SafeOutput has in common,
// regardless of mechanism: "actually do the write, however that
// works for you." Transport-specific plumbing (an iTransport&) lives
// one level down, in TransportSafeOutput — a GPIO-backed SafeOutput
// derives from THIS class directly instead, since it has no
// transport to hold.
class SafeOutput : public Safe {
public:
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

    // Sets the new desired safety state and actually performs the
    // write to reflect it — this is the generic entry point anything
    // holding a plain SafeOutput& (SafeDevice, say) uses to drive it,
    // without needing to know the concrete type underneath. Promoted
    // here from being a GpioSafeOutput-specific method: a generic
    // caller holding only a SafeOutput& had no way to actually tell
    // it "the decision changed" without this being part of the
    // abstract contract itself.
    virtual void setDesiredState(bool safe) = 0;

    // Re-asserts whatever setDesiredState() last set, without a new
    // decision — useful for periodic re-driving (e.g. in case
    // something else on the bus could glitch an output and it's
    // worth re-writing it periodically regardless of whether the
    // decision itself has changed). HOW to actually perform the
    // write is entirely mechanism-specific (a GPIO pin write, a CAN
    // frame, an MQTT publish...) — a concrete subclass implements
    // this however its situation actually requires.
    virtual bool send() = 0;

protected:
    void setSafe1State(bool newState) { reportState(safe1_, safe1Callback_, safe1CallbackContext_, newState); }
    void setSafe2State(bool newState) { reportState(safe2_, safe2Callback_, safe2CallbackContext_, newState); }

private:
    void reportState(bool& stateField, SafeCallback callback, void* context, bool newState) {
        const bool changed = (stateField != newState);
        stateField = newState;
        if (changed && callback) callback(*this, context);
    }

    bool         safe1_ = false;
    bool         safe2_ = false;
    SafeCallback safe1Callback_ = nullptr;
    SafeCallback safe2Callback_ = nullptr;
    void*        safe1CallbackContext_ = nullptr;
    void*        safe2CallbackContext_ = nullptr;
};
