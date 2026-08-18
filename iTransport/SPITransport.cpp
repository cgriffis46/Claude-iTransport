#include "SPITransport.h"

// NOTE: deliberately NOT calling halCsHigh() here to idle the line —
// it's pure virtual, and calling a pure virtual from a base class's
// own constructor is undefined behavior (the derived class's vtable
// isn't wired up yet at this point). Stm32HalSPITransport's own
// constructor handles the idle-high setup instead, where calling its
// own override is safe.
SPITransport::SPITransport(void* spiHandle, void* csPort, uint16_t csPin)
    : spiHandle_(spiHandle), csPort_(csPort), csPin_(csPin) {}

bool SPITransport::writeReg(uint8_t reg, uint8_t value) {
    if (busy_) return false;
    failed_  = false;
    isRead_  = false;
    xferLen_ = 2;
    txBuf_[0] = reg & static_cast<uint8_t>(~kReadBit);
    txBuf_[1] = value;

    halCsLow();
    busy_ = halTransmit(txBuf_, xferLen_);
    if (!busy_) halCsHigh(); // failed to even start — don't leave CS asserted
    return busy_;
}

bool SPITransport::readRegs(uint8_t reg, uint8_t* buf, uint8_t len) {
    if (busy_) return false;
    if (static_cast<uint16_t>(len) + 1 > kMaxLen) return false; // stays within scratch buffers

    failed_  = false;
    isRead_  = true;
    destBuf_ = buf;
    xferLen_ = static_cast<uint8_t>(len + 1);

    txBuf_[0] = reg | kReadBit;
    for (uint8_t i = 1; i < xferLen_; ++i) txBuf_[i] = 0xFF; // dummy bytes to drive the clock

    halCsLow();
    busy_ = halTransmitReceive(txBuf_, rxBuf_, xferLen_);
    if (!busy_) halCsHigh();
    return busy_;
}

bool SPITransport::isBusy() const {
    if (!busy_) return false;
    if (halIsReady()) {
        auto* self = const_cast<SPITransport*>(this);
        self->halCsHigh();
        self->failed_ = halLastTransferFailed();

        if (self->isRead_ && !self->failed_) {
            // rxBuf_[0] captured garbage while we clocked out the address
            // byte — the real data starts at index 1.
            for (uint8_t i = 0; i + 1 < self->xferLen_; ++i) {
                self->destBuf_[i] = self->rxBuf_[i + 1];
            }
        }
        self->busy_ = false;
    }
    return busy_;
}

bool SPITransport::lastOpFailed() const { return failed_; }

bool SPITransport::checkDevice() {
    // No bus-level presence signal exists for SPI. A sensor using this
    // transport that wants a real check should read its chip-ID
    // register (e.g. BMP280 0xD0 == 0x58) and compare it itself.
    return true;
}
