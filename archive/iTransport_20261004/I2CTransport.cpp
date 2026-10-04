#include "I2CTransport.h"
#include <cstddef>

namespace {
    // Small fixed registry mapping an opaque bus handle to whichever
    // I2CTransport instance currently has a transfer pending on it —
    // the mutex guarantees at most one at a time per bus. Bump
    // kMaxBuses if a project has more than 4 I2C peripherals.
    constexpr size_t kMaxBuses = 4;
    void*          s_busHandle[kMaxBuses]   = {nullptr, nullptr, nullptr, nullptr};
    I2CTransport*  s_activeOnBus[kMaxBuses] = {nullptr, nullptr, nullptr, nullptr};
}

void I2CTransport::registerActive(void* busHandle, I2CTransport* instance) {
    for (size_t i = 0; i < kMaxBuses; ++i) {
        if (s_busHandle[i] == nullptr || s_busHandle[i] == busHandle) {
            s_busHandle[i]   = busHandle;
            s_activeOnBus[i] = instance;
            return;
        }
    }
    // registry full — bump kMaxBuses if this project has more I2C buses
}

void I2CTransport::unregisterActive(void* busHandle) {
    for (size_t i = 0; i < kMaxBuses; ++i) {
        if (s_busHandle[i] == busHandle) {
            s_activeOnBus[i] = nullptr;
            return;
        }
    }
}

// Finds the instance currently mid-transfer on this bus and calls its
// SignalTransferComplete() — default just sets a flag; an overridden
// (RTOS) version wakes a waiting task directly.
void I2CTransport::onTransferComplete(void* busHandle, bool failed) {
    for (size_t i = 0; i < kMaxBuses; ++i) {
        if (s_busHandle[i] == busHandle && s_activeOnBus[i] != nullptr) {
            s_activeOnBus[i]->SignalTransferComplete(failed);
            return;
        }
    }
}

void I2CTransport::SignalTransferComplete(bool failed) {
    transferNotified_       = true;
    transferNotifiedFailed_ = failed;
}

bool I2CTransport::WaitForTransfer(uint32_t timeoutTicks) {
    for (uint32_t i = 0; i < timeoutTicks; ++i) {
        if (transferNotified_) {
            failed_            = transferNotifiedFailed_;
            transferNotified_  = false;
            return true;
        }
        YieldTick();
    }
    // one last check after the final yield, same as the loop body
    if (transferNotified_) {
        failed_           = transferNotifiedFailed_;
        transferNotified_ = false;
        return true;
    }
    return false; // timed out — still busy
}

I2CTransport::I2CTransport(void* busHandle, uint8_t deviceAddr7bit, void* busMutex)
    : busHandle_(busHandle), devAddr8bit_(static_cast<uint8_t>(deviceAddr7bit << 1)), busMutex_(busMutex) {}

bool I2CTransport::writeReg(uint8_t reg, uint8_t value) {
    if (busy_) return false; // this instance already has an outstanding transfer

    if (!ObtainMutex(kMutexTimeoutTicks)) {
        return false; // bus busy elsewhere; caller's sleep()-then-retry handles this
    }

    failed_ = false;
    writeByte_ = value;
    registerActive(busHandle_, this);

    if (!halMemWrite(reg, &writeByte_, 1)) {
        unregisterActive(busHandle_);
        ReleaseMutex();
        return false;
    }

    busy_ = true;
    return true;
}

bool I2CTransport::readRegs(uint8_t reg, uint8_t* buf, uint8_t len) {
    if (busy_) return false;

    if (!ObtainMutex(kMutexTimeoutTicks)) {
        return false;
    }

    failed_ = false;
    registerActive(busHandle_, this);

    if (!halMemRead(reg, buf, len)) {
        unregisterActive(busHandle_);
        ReleaseMutex();
        return false;
    }

    busy_ = true;
    return true;
}

bool I2CTransport::isBusy() const {
    if (!busy_) return false;

    auto* self = const_cast<I2CTransport*>(this);

    // Bounded wait for the notification from onTransferComplete() —
    // default is a busy-wait poll yielding via YieldTick(); an
    // overridden (RTOS) version blocks properly instead. Either way,
    // this call also updates failed_ once resolved.
    if (!self->WaitForTransfer(kTransferTimeoutTicks)) {
        return true; // timed out — still busy as far as the caller's concerned
    }

    self->busy_ = false;
    unregisterActive(busHandle_);
    self->ReleaseMutex();
    return false;
}

bool I2CTransport::lastOpFailed() const { return failed_; }

// checkDevice() still has to take the mutex first so it can't collide
// with another task's in-flight transfer on the same bus — the actual
// "is anyone there" check is halIsDeviceReady() (Stm32HalI2CTransport),
// which may itself be a blocking HAL call; that's the concrete
// class's business, not this one's.
bool I2CTransport::checkDevice() {
    if (!ObtainMutex(kMutexTimeoutTicks)) {
        failed_ = true;
        return false;
    }
    const bool ready = halIsDeviceReady(/*Trials=*/2, /*Timeout=*/10);
    ReleaseMutex();
    failed_ = !ready;
    return ready;
}
