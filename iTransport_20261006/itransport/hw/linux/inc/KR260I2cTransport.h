#pragma once
#include "IFileTransport.h"

// Linux i2c-dev based I2C, for the KR260's Cortex-A53 application
// cores running PetaLinux/Ubuntu — NOT the RPU (Cortex-R5) real-time
// cores, which would run bare-metal/FreeRTOS with a different driver
// entirely. Talks to /dev/i2c-N via the standard i2c-dev ioctl
// interface (I2C_RDWR + struct i2c_msg), which is what any embedded
// Linux userspace I2C driver uses.
//
// Built on IFileTransport, not FreeRtosTransport<I2CTransport>: this class
// assumes a plain Linux userspace with no CMSIS-RTOS2-compatible
// layer available — genuinely the same situation as WiringPiI2cTransport
// (both are POSIX file descriptors, both synchronous — ioctl(fd,
// I2C_RDWR, ...) blocks until the transfer completes, so there's no
// "later" to signal completion for). fd_ ownership and
// close-on-destruction come from IFileTransport; this class only
// supplies the actual ioctl calls.
//
// Cross-device bus sharing is handled by IFileTransport's POSIX
// semaphore (ObtainMutex()/ReleaseMutex(), wrapped around every
// method below) — pass the SAME sem_t* to every KR260I2cTransport on
// the same /dev/i2c-N node.
class KR260I2cTransport : public IFileTransport {
public:
    // devicePath is the i2c-dev node, e.g. "/dev/i2c-1" — KR260's
    // PMOD/Raspberry Pi header exposes specific bus numbers; check
    // `i2cdetect -l` on the target to confirm which one you want.
    KR260I2cTransport(const char* devicePath, uint8_t deviceAddr7bit, sem_t* busSemaphore);

    bool writeReg(uint8_t reg, uint8_t value) override;
    bool writeRegs(uint8_t reg, const uint8_t* buf, uint8_t len) override;
    bool readRegs(uint8_t reg, uint8_t* buf, uint8_t len) override;
    bool writeBytes(const uint8_t* buf, uint8_t len) override;
    bool readBytes(uint8_t* buf, uint8_t len) override;
    bool checkDevice() override;

private:
    // One I2C_RDWR transaction: wlen bytes written, then (after a
    // repeated start, no STOP in between) rlen bytes read. Either
    // half may be absent (length 0). Takes and releases the bus
    // semaphore and sets failed_.
    bool transfer(uint8_t* wbuf, uint16_t wlen, uint8_t* rbuf, uint16_t rlen);

    uint8_t devAddr7bit_;
};
