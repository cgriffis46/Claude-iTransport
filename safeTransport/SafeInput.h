#pragma once
#include "Safe.h"

// Abstract base for reading a safety-relevant value from ANYWHERE —
// deliberately not even committed to "there's a transport involved at
// all". A SafeInput could be backed by a byte-stream transport (CAN,
// Ethernet, RS485, UART...), a PLC tag read over Modbus or
// EtherNet/IP, an MQTT subscription, or something with no "transport"
// concept in the conventional sense whatsoever — a GPIO input pin,
// just a digital voltage level, no bytes involved at all.
//
// An earlier version of this class required an iTransport& directly
// in its own constructor — too narrow: a GPIO-backed SafeInput has
// nothing resembling a byte stream to receive, and forcing one into
// this shape would mean inventing a fake "transport" purely to
// satisfy the constructor. That transport-specific plumbing now
// lives in TransportSafeInput (a SafeInput that specifically IS
// backed by an iTransport), leaving THIS base free of any assumption
// about how a concrete subclass actually gets its data — a
// GPIO-backed one derives from SafeInput directly; a CAN/Ethernet/
// UART-backed one derives from TransportSafeInput instead.
//
// Genuine open design question, not silently resolved: Safe's own
// shape (Safe1 AND Safe2, two independent channels of ONE decision)
// doesn't map cleanly onto a SINGLE physical signal — e.g. one UART
// loopback channel, standing alone as its own SafeInput. This base
// makes no assumption either way; a concrete single-channel SafeInput
// is free to report the same value on both GetSafe1State() and
// GetSafe2State() (the convention used for the UART-channel SafeInput
// built alongside this class), or to interpret the two differently if
// that fits its own situation better.
class SafeInput : public Safe {
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

protected:
    // Concrete subclasses call these once they've determined a new
    // state, however they actually determine it (a byte stream, a
    // polled GPIO read, an MQTT message callback, a PLC tag poll) —
    // handles change detection and callback firing once, shared,
    // rather than each subclass reimplementing it.
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
