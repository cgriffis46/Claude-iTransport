#include "ArduinoWireTransport.h"

// devAddr8bit_ (inherited, protected) is pre-shifted for HAL's 8-bit
// convention; Wire wants the plain 7-bit address back.
static inline uint8_t sevenBit(uint8_t addr8bit) { return addr8bit >> 1; }

bool ArduinoWireTransport::halMemWrite(uint8_t reg, uint8_t* pData, uint16_t size) {
    wire_.beginTransmission(sevenBit(devAddr8bit_));
    wire_.write(reg);
    wire_.write(pData, size);
    const bool ok = (wire_.endTransmission() == 0); // blocking — Wire has no IT-style API

    // Wire already blocked until this finished, so there's no async
    // completion to wait for later — signal it right now rather than
    // leaving isBusy() waiting on a notification nothing else will send.
    I2CTransport::onTransferComplete(&wire_, !ok);
    return true; // "issued" — which for Wire also means "already done"
}

bool ArduinoWireTransport::halMemRead(uint8_t reg, uint8_t* pData, uint16_t size) {
    wire_.beginTransmission(sevenBit(devAddr8bit_));
    wire_.write(reg);
    // false = repeated start, keep the bus held for the read that follows
    if (wire_.endTransmission(false) != 0) {
        I2CTransport::onTransferComplete(&wire_, true);
        return true;
    }

    const uint8_t got = wire_.requestFrom(sevenBit(devAddr8bit_), size);
    for (uint8_t i = 0; i < got && i < size; ++i) {
        pData[i] = wire_.read();
    }
    const bool ok = (got == size);

    I2CTransport::onTransferComplete(&wire_, !ok);
    return true;
}

bool ArduinoWireTransport::halIsDeviceReady(uint32_t /*trials*/, uint32_t /*timeout*/) {
    wire_.beginTransmission(sevenBit(devAddr8bit_));
    return wire_.endTransmission() == 0;
}
