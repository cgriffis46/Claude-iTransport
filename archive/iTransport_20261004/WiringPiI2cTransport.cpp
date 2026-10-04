#include "WiringPiI2cTransport.h"
#include <wiringPiI2C.h>

WiringPiI2cTransport::WiringPiI2cTransport(uint8_t deviceAddr7bit, sem_t* busSemaphore)
    : WiringPiTransport(wiringPiI2CSetup(deviceAddr7bit), busSemaphore) {}

bool WiringPiI2cTransport::writeReg(uint8_t reg, uint8_t value) {
    if (fd_ < 0) {
        failed_ = true;
        return false;
    }
    if (!ObtainMutex()) {
        failed_ = true;
        return false;
    }

    // wiringPiI2CWriteReg8() already blocked until done — "issued"
    // and "landed" are the same thing here, unlike the STM32 HAL path.
    const bool ok = (wiringPiI2CWriteReg8(fd_, reg, value) >= 0);
    ReleaseMutex();
    failed_ = !ok;
    return ok;
}

bool WiringPiI2cTransport::readRegs(uint8_t reg, uint8_t* buf, uint8_t len) {
    if (fd_ < 0) {
        failed_ = true;
        return false;
    }
    if (!ObtainMutex()) {
        failed_ = true;
        return false;
    }

    // wiringPiI2CReadReg8() reads exactly one register per call — an
    // SMBus-style single-byte access, not a burst/block read — so a
    // multi-byte readRegs() loops it, incrementing the register
    // address each call. The WHOLE loop is one ObtainMutex()/
    // ReleaseMutex() pair, not one per byte — otherwise another
    // thread's transaction could interleave in the middle of what's
    // meant to be one logical multi-byte read. That issues `len`
    // separate I2C transactions rather than one atomic burst with a
    // repeated start, but at least nothing else on this bus can cut
    // in partway through. Correct in practice for BMP280's
    // calibration table and measurement registers; if true burst
    // atomicity ever matters for a future sensor, drop to a raw
    // ioctl(fd_, I2C_RDWR, ...) call instead, same technique as
    // KR260I2cTransport.
    bool ok = true;
    for (uint8_t i = 0; i < len && ok; ++i) {
        const int value = wiringPiI2CReadReg8(fd_, static_cast<int>(reg + i));
        if (value < 0) {
            ok = false;
        } else {
            buf[i] = static_cast<uint8_t>(value);
        }
    }

    ReleaseMutex();
    failed_ = !ok;
    return ok;
}

// No dedicated "is device ready" call in WiringPi's I2C API — a
// 1-byte probe read is the same technique KR260I2cTransport uses for
// the same reason (Linux's i2c-dev, which WiringPi sits on top of,
// has no direct equivalent either).
bool WiringPiI2cTransport::checkDevice() {
    if (fd_ < 0) return false;
    if (!ObtainMutex()) return false;

    const bool ready = wiringPiI2CRead(fd_) >= 0;
    ReleaseMutex();
    return ready;
}
