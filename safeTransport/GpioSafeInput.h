#pragma once
#include "stm32f4xx_hal.h"
#include "SafeInput.h"

// A GPIO digital input pin, standing alone as its own SafeInput —
// derives from SafeInput directly, NOT TransportSafeInput, same
// reasoning as GpioSafeOutput: no transport, no bytes, just a
// voltage level read directly.
//
// Polled, not interrupt-driven: HAL_GPIO_ReadPin() is a simple,
// synchronous register read — there's no equivalent "byte arrived"
// event the way a real transport has, so this uses poll() (same
// convention as UartLoopbackChannelSafeInput's own poll()) rather
// than a push-based callback. A future EXTI-based (external-
// interrupt) variant could exist alongside this one if some use case
// needs the pin change noticed without waiting for the next poll() —
// not built here, since nothing has asked for it yet.
//
// Typical use (the dual-MCU cross-check): this reads a pin driven by
// a SEPARATE microcontroller's own GpioSafeOutput — one MCU's output
// wired to the other's input, and vice versa — so each MCU's own
// SafeDevice combines its own local health (e.g. a UART self-
// loopback) with whether the OTHER MCU currently agrees things are
// safe. A fault on either MCU then propagates to both, via
// SafeInterlock's own "channels must agree" requirement.
class GpioSafeInput : public SafeInput {
public:
    // port/pin must already be configured as a GPIO input via
    // CubeMX before this is constructed. activeHigh: true means
    // GPIO_PIN_SET reads as "safe"; false inverts that, for an
    // active-low signal.
    GpioSafeInput(GPIO_TypeDef* port, uint16_t pin, bool activeHigh = true);

    // Call periodically — reads the pin and updates Safe1/Safe2 (both
    // set to the same value, same single-channel convention as
    // UartLoopbackChannelSafeInput).
    void poll();

private:
    GPIO_TypeDef* port_;
    uint16_t      pin_;
    bool          activeHigh_;
};
