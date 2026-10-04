#pragma once
#include "WiringPiTransport.h"

// I2C via WiringPi's high-level wiringPiI2C* functions — synchronous,
// like every other userspace I2C library covered in this codebase
// (Arduino Wire, Linux i2c-dev raw ioctl). No mutex/notification
// machinery needed: by the time writeReg()/readRegs() return, the
// transfer already happened (see WiringPiTransport for why).
class WiringPiI2cTransport : public WiringPiTransport {
public:
    // deviceAddr7bit is the sensor's I2C address. wiringPiI2CSetup()
    // auto-detects which physical bus to use based on the Raspberry
    // Pi board revision, and internally does the equivalent of
    // ioctl(fd, I2C_SLAVE, deviceAddr7bit) for us. The resulting fd is
    // owned by IFileTransport now (fd_, protected, inherited) — this
    // class doesn't need its own cleanup; ~IFileTransport() handles it.
    // busSemaphore must be the SAME sem_t* for every IFileTransport
    // sharing this physical bus — see IFileTransport's own comment.
    WiringPiI2cTransport(uint8_t deviceAddr7bit, sem_t* busSemaphore);

    bool writeReg(uint8_t reg, uint8_t value) override;
    bool readRegs(uint8_t reg, uint8_t* buf, uint8_t len) override;
    bool checkDevice() override;
};
