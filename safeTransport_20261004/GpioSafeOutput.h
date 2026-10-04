#pragma once
#include "stm32f4xx_hal.h"
#include "SafeOutput.h"

// A GPIO digital output pin, driven by an upstream safety decision —
// derives from SafeOutput directly, NOT TransportSafeOutput, since a
// GPIO pin has no transport at all: no bytes, no write(), just a
// voltage level. This is exactly the case that motivated splitting
// SafeOutput's transport-specific plumbing into TransportSafeOutput
// in the first place.
//
// Typical use (matching the two-board proof of concept): something
// upstream — a CAN-based SafeInput receiving Board 1's safety state,
// say — calls setDesiredState() whenever that upstream state changes.
// This both updates GpioSafeOutput's own reported Safe1/Safe2 state
// (via SafeOutput's shared setSafe1State()/setSafe2State() helpers)
// and writes the physical pin to match.
class GpioSafeOutput : public SafeOutput {
public:
    // port/pin must already be configured as a GPIO output via
    // CubeMX before this is constructed.
    GpioSafeOutput(GPIO_TypeDef* port, uint16_t pin);

    // Call whenever the upstream safety decision changes. Updates
    // this object's own Safe1/Safe2 state (both set to the same
    // value — see SafeInput's header comment on the same single-
    // channel convention) and writes the pin to match.
    void setDesiredState(bool safe) override;

    // Re-asserts the pin to match whatever setDesiredState() last set,
    // without requiring a new decision — useful for periodic
    // re-driving rather than relying on a single write persisting
    // (e.g. if something else on the bus could glitch the pin).
    bool send() override;

private:
    GPIO_TypeDef* port_;
    uint16_t      pin_;
    bool          desiredState_ = false;
};
