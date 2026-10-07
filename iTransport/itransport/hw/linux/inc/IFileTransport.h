#pragma once
#include <semaphore.h>
#include "ISensorTransport.h"

// Shared base for any transport built on a POSIX file descriptor —
// the "everything is a file" Unix/Linux idea, applied here: Linux
// exposes I2C and SPI buses as character devices (/dev/i2c-N,
// /dev/spidev-N.N), and libraries like WiringPi that sit on top of
// them are themselves just thin wrappers around that same file
// descriptor (wiringPiI2CSetup() hands one straight back).
//
// Every operation through a POSIX fd (read/write/ioctl) is
// synchronous/blocking — there's no async/interrupt-driven mode the
// way STM32 HAL has — so isBusy() is trivially "already done" the
// instant a call returns, true for any file-descriptor-backed
// transport, not something each concrete class needs to reimplement.
//
// Owns fd_ and its lifetime: the destructor closes it if it's open,
// so a concrete subclass doesn't need to repeat that cleanup itself.
//
// busSemaphore must be a POSIX unnamed semaphore, sem_init()'d with
// an initial count of 1 (i.e. used as a binary mutex) — shared by
// every IFileTransport-derived instance that talks to the SAME
// physical bus. Create it once per bus, the same way I2CTransport's
// busMutex_ works for the CMSIS-RTOS2 side. Unlike I2CTransport,
// which calls ObtainMutex()/ReleaseMutex() automatically from its own
// writeReg()/readRegs(), this base doesn't call them for you — there's
// no shared "issue, then wait for completion" shape to hook into,
// since writeReg()/readRegs()/checkDevice() are each still a
// subclass's own pure-virtual implementation. A concrete transport
// wraps its own bodies with ObtainMutex()/ReleaseMutex() to actually
// get real cross-instance exclusion.
class IFileTransport : public ISensorTransport {
public:
    ~IFileTransport() override;

    bool isBusy() const override { return false; }
    bool lastOpFailed() const override { return failed_; }

protected:
    IFileTransport(int fd, sem_t* busSemaphore) : fd_(fd), busSemaphore_(busSemaphore) {}

    bool isOpen() const { return fd_ >= 0; }

    // Blocking sem_wait()/sem_post(). No bounded/timeout variant yet
    // — sem_timedwait() would be the natural extension if a concrete
    // transport ever needs one.
    bool ObtainMutex();
    void ReleaseMutex();

    int    fd_;
    sem_t* busSemaphore_;
    bool   failed_ = false;
};
