#include "KR260I2cTransport.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>

KR260I2cTransport::KR260I2cTransport(const char* devicePath, uint8_t deviceAddr7bit, sem_t* busSemaphore)
    : IFileTransport(::open(devicePath, O_RDWR), busSemaphore), devAddr7bit_(deviceAddr7bit) {}

// ioctl() blocks until the transaction has finished — "issued" and
// "done" are the same thing here, unlike the STM32 HAL path.
bool KR260I2cTransport::transfer(uint8_t* wbuf, uint16_t wlen, uint8_t* rbuf, uint16_t rlen) {
    if (!isOpen()) {
        failed_ = true;
        return false;
    }
    if (!ObtainMutex()) {
        failed_ = true;
        return false;
    }

    struct i2c_msg msgs[2] = {};
    int n = 0;
    if (wlen > 0) {
        msgs[n].addr  = devAddr7bit_;
        msgs[n].flags = 0; // write
        msgs[n].len   = wlen;
        msgs[n].buf   = wbuf;
        ++n;
    }
    if (rlen > 0) {
        msgs[n].addr  = devAddr7bit_;
        msgs[n].flags = I2C_M_RD; // after a write this is a repeated start, no STOP between
        msgs[n].len   = rlen;
        msgs[n].buf   = rbuf;
        ++n;
    }

    struct i2c_rdwr_ioctl_data packets{};
    packets.msgs  = msgs;
    packets.nmsgs = static_cast<__u32>(n);

    const bool ok = (n > 0) && (::ioctl(fd_, I2C_RDWR, &packets) >= 0);
    ReleaseMutex();
    failed_ = !ok;
    return ok;
}

bool KR260I2cTransport::writeReg(uint8_t reg, uint8_t value) {
    return writeRegs(reg, &value, 1);
}

bool KR260I2cTransport::writeRegs(uint8_t reg, const uint8_t* buf, uint8_t len) {
    if (buf == nullptr || len == 0 || len > kMaxWriteLen) return false;

    uint8_t out[1 + kMaxWriteLen];
    out[0] = reg;
    for (uint8_t i = 0; i < len; ++i) out[i + 1] = buf[i];
    return transfer(out, static_cast<uint16_t>(len + 1), nullptr, 0);
}

bool KR260I2cTransport::readRegs(uint8_t reg, uint8_t* buf, uint8_t len) {
    if (buf == nullptr || len == 0) return false;
    return transfer(&reg, 1, buf, len); // write the register address first
}

bool KR260I2cTransport::writeBytes(const uint8_t* buf, uint8_t len) {
    if (buf == nullptr || len == 0 || len > kMaxWriteLen) return false;

    uint8_t out[kMaxWriteLen]; // i2c_msg wants a non-const buffer
    for (uint8_t i = 0; i < len; ++i) out[i] = buf[i];
    return transfer(out, len, nullptr, 0);
}

bool KR260I2cTransport::readBytes(uint8_t* buf, uint8_t len) {
    if (buf == nullptr || len == 0) return false;
    return transfer(nullptr, 0, buf, len);
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
