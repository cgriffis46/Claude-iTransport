#include "FreeRtosI2CTransport.h"

// busMutex_ (inherited, protected, void*) always actually holds an
// osMutexId_t here — this is the one place that's true, since only a
// FreeRtosI2CTransport-derived class is ever constructed with a real
// FreeRTOS mutex to begin with. Also captures which task is about to
// issue a transfer, so SignalTransferComplete() (called later, likely
// from an ISR) knows who to wake.
bool FreeRtosI2CTransport::ObtainMutex(uint32_t timeoutTicks) {
    if (osMutexAcquire(static_cast<osMutexId_t>(busMutex_), timeoutTicks) != osOK) {
        return false;
    }
    waitingTask_ = osThreadGetId();
    return true;
}

void FreeRtosI2CTransport::ReleaseMutex() {
    osMutexRelease(static_cast<osMutexId_t>(busMutex_));
}

void FreeRtosI2CTransport::SignalTransferComplete(bool failed) {
    if (waitingTask_) {
        osThreadFlagsSet(waitingTask_, failed ? kFlagFailed : kFlagDone);
    }
}

bool FreeRtosI2CTransport::WaitForTransfer(uint32_t timeoutTicks) {
    // Bounded block on the notification from SignalTransferComplete()
    // (ISR context, typically). Woken immediately once the transfer
    // lands, instead of polling — the RTOS-backed version of the
    // default busy-wait in I2CTransport.
    const uint32_t flags = osThreadFlagsWait(kFlagsMask, osFlagsWaitAny, timeoutTicks);
    if (flags & osFlagsError) {
        return false; // timeout (or other RTOS error) — still busy
    }
    failed_ = (flags & kFlagFailed) != 0;
    return true;
}
