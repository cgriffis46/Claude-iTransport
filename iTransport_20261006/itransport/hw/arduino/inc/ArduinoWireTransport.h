#pragma once
#include <Arduino.h> // delay()
#include <Wire.h>
#include "I2CTransport.h"

// Wire-based I2C, for any Arduino-API target — deliberately derives
// from I2CTransport directly, NOT FreeRtosTransport<I2CTransport>, so this works
// on a plain AVR Uno/Mega/Nano with no RTOS at all, not just on
// something like ESP32's FreeRTOS-backed core.
//
// Wire has no true non-blocking transfer API — beginTransmission()/
// endTransmission()/requestFrom() all block until finished. So
// halMemWrite()/halMemRead() below signal completion immediately,
// right after the blocking Wire call returns, via the default
// SignalTransferComplete()/WaitForTransfer() pair (a plain flag,
// checked by isBusy()) — there's no real RTOS notification to use
// instead, and none is needed: by the time isBusy() is first called,
// the flag set here has almost always already resolved things, since
// Wire already did the actual waiting internally.
//
// YieldTick() below overrides the no-op default with a real blocking
// delay(1) — the fallback this class was specifically asked to use
// "if a non-blocking alternative is unavailable." In practice it
// should rarely if ever actually fire, given the synchronous
// self-signaling above, but it's the correct thing to have in place
// for whatever timing edge case WaitForTransfer()'s busy-wait loop
// might still hit.
//
// BusTransport's registry (see onTransferComplete()) is
// keyed on an opaque void* — this class just passes &wire through
// directly, no cast needed at all.
class ArduinoWireTransport : public I2CTransport {
public:
    ArduinoWireTransport(TwoWire& wire, uint8_t deviceAddr7bit)
        : I2CTransport(&wire, deviceAddr7bit, /*busMutex=*/nullptr), wire_(wire) {}

protected:
    bool halMemWrite(uint8_t reg, uint8_t* pData, uint16_t size) override;
    bool halMemRead(uint8_t reg, uint8_t* pData, uint16_t size) override;
    bool halMasterTransmit(uint8_t* pData, uint16_t size) override;
    bool halMasterReceive(uint8_t* pData, uint16_t size) override;
    bool halIsDeviceReady(uint32_t trials, uint32_t timeout) override;
    void YieldTick() override { delay(1); }

private:
    TwoWire& wire_;
};
