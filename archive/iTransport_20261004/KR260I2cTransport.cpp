#include "KR260I2cTransport.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>

KR260I2cTransport::KR260I2cTransport(const char* devicePath, uint8_t deviceAddr7bit, sem_t* busSemaphore)
    : IFileTransport(::open(devicePath, O_RDWR), busSemaphore), devAddr7bit_(deviceAddr7bit) {}

bool KR260I2cTransport::writeReg(uint8_t reg, uint8_t value) {
    if (!isOpen()) {
        failed_ = true;
        return false;
    }
    if (!ObtainMutex()) {
        failed_ = true;
        return false;
    }

    uint8_t buf[2] = { reg, value };

    struct i2c_msg msg{};
    msg.addr  = devAddr7bit_;
    msg.flags = 0; // write
    msg.len   = sizeof(buf);
    msg.buf   = buf;

    struct i2c_rdwr_ioctl_data packets{};
    packets.msgs  = &msg;
    packets.nmsgs = 1;

    // ioctl() already blocked until this finished — "issued" and
    // "done" are the same thing here, unlike the STM32 HAL path.
    const bool ok = (::ioctl(fd_, I2C_RDWR, &packets) >= 0);
    ReleaseMutex();
    failed_ = !ok;
    return ok;
}

bool KR260I2cTransport::readRegs(uint8_t reg, uint8_t* buf, uint8_t len) {
    if (!isOpen()) {
        failed_ = true;
        return false;
    }
    if (!ObtainMutex()) {
        failed_ = true;
        return false;
    }

    uint8_t regBuf[1] = { reg };

    struct i2c_msg msgs[2] = {};
    msgs[0].addr  = devAddr7bit_;
    msgs[0].flags = 0; // write the register address first
    msgs[0].len   = 1;
    msgs[0].buf   = regBuf;

    msgs[1].addr  = devAddr7bit_;
    msgs[1].flags = I2C_M_RD; // repeated start, no STOP between the two messages
    msgs[1].len   = len;
    msgs[1].buf   = buf;

    struct i2c_rdwr_ioctl_data packets{};
    packets.msgs  = msgs;
    packets.nmsgs = 2;

    const bool ok = (::ioctl(fd_, I2C_RDWR, &packets) >= 0);
    ReleaseMutex();
    failed_ = !ok;
    return ok;
}

// Linux has no equivalent of HAL_I2C_IsDeviceReady(). A 1-byte probe
// read is the common technique (same idea `i2cdetect` uses under the
// hood) — success means something acknowledged the address.
bool KR260I2cTransport::checkDevice() {
    if (!isOpen()) return false;
    if (!ObtainMutex()) return false;

    uint8_t probe = 0;
    struct i2c_msg msg{};
    msg.addr  = devAddr7bit_;
    msg.flags = I2C_M_RD;
    msg.len   = 1;
    msg.buf   = &probe;

    struct i2c_rdwr_ioctl_data packets{};
    packets.msgs  = &msg;
    packets.nmsgs = 1;

    const bool ready = ::ioctl(fd_, I2C_RDWR, &packets) >= 0;
    ReleaseMutex();
    return ready;
}
