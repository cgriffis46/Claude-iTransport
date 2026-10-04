#pragma once
#include "cmsis_os2.h"
#include "I2CTransport.h"

// Provides the real CMSIS-RTOS2 calls that I2CTransport's default
// mutex/notification hooks are no-op/busy-wait fallbacks for:
//   - ObtainMutex()/ReleaseMutex()               -> osMutexAcquire()/osMutexRelease()
//   - SignalTransferComplete()/WaitForTransfer() -> osThreadFlagsSet()/osThreadFlagsWait()
// Still abstract — halMemWrite()/halMemRead()/halIsDeviceReady() (the
// actual hardware calls) are left for a further-derived class
// (Stm32HalI2CTransport, KR260I2cTransport, ...) to supply.
//
// Every concrete I2C transport that has a real FreeRTOS (or other
// CMSIS-RTOS2-compatible) target derives from THIS instead of
// I2CTransport directly, so these calls exist in exactly one place.
// A transport with no such RTOS (e.g. a plain AVR Arduino) derives
// from I2CTransport directly instead and gets the busy-wait/no-op
// defaults — see ArduinoWireTransport's YieldTick() override.
class FreeRtosI2CTransport : public I2CTransport {
public:
    using I2CTransport::I2CTransport;

protected:
    bool ObtainMutex(uint32_t timeoutTicks) override;
    void ReleaseMutex() override;

    void SignalTransferComplete(bool failed) override;
    bool WaitForTransfer(uint32_t timeoutTicks) override;

private:
    static constexpr uint32_t kFlagDone   = 0x1;
    static constexpr uint32_t kFlagFailed = 0x2;
    static constexpr uint32_t kFlagsMask  = kFlagDone | kFlagFailed;

    osThreadId_t waitingTask_ = nullptr;
};
