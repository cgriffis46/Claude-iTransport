#pragma once
#include "ISensorTransport.h"

// Generic I2C transport: owns the mutex-based bus arbitration, the
// transfer-completion notification, and the busy/failed bookkeeping
// ISensorTransport requires — all hardware-call-agnostic AND
// platform/RTOS-agnostic. No RTOS header is included here at all
// anymore: ObtainMutex()/ReleaseMutex() and SignalTransferComplete()/
// WaitForTransfer() are where any real platform's primitives
// actually happen (see FreeRtosI2CTransport for the CMSIS-RTOS2
// versions); the defaults below are a genuinely working, if less
// efficient, fallback — a plain flag plus a busy-wait loop, yielding
// via YieldTick() (a no-op unless overridden, e.g. Arduino's delay(1)).
//
// Every literal STM32 call (HAL_I2C_Mem_Write_IT, HAL_I2C_Mem_Read_IT,
// HAL_I2C_IsDeviceReady) lives in Stm32HalI2CTransport, and so does
// stm32f4xx_hal.h itself — this header has no STM32 type in it
// anywhere. That's not just tidiness: many STM32Cube HAL versions
// typedef I2C_HandleTypeDef from an ANONYMOUS struct (no tag name),
// which means forward-declaring it here (`struct I2C_HandleTypeDef;`)
// would conflict with the real definition once Stm32HalI2CTransport.h
// includes it — a genuine redefinition error, not just a style
// concern. Storing the bus handle as an opaque void* avoids that
// entirely.
class I2CTransport : public ISensorTransport {
public:
    // busHandle should be the bus's real HAL handle (e.g. &hi2c1),
    // passed through as void* — the concrete transport casts it back.
    // busMutex is likewise opaque (see ObtainMutex()/ReleaseMutex()
    // below) — must be the SAME value for every I2CTransport sharing
    // this physical bus, create it once per bus and pass it to each
    // device's transport. Pass nullptr for either if this platform
    // has no real mutex/handle concept — the defaults handle that.
    I2CTransport(void* busHandle, uint8_t deviceAddr7bit, void* busMutex);
    ~I2CTransport() override = default;

    bool writeReg(uint8_t reg, uint8_t value) override;
    bool readRegs(uint8_t reg, uint8_t* buf, uint8_t len) override;
    bool isBusy() const override;
    bool lastOpFailed() const override;
    bool checkDevice() override;

    // Call this from wherever this platform learns a transfer
    // finished — an HAL ISR callback (HAL_I2C_MemTxCpltCallback etc.)
    // on STM32, or a direct call right after a blocking Wire/ioctl
    // call returns (see ArduinoWireTransport/KR260I2cTransport).
    // Finds whichever I2CTransport instance currently has a transfer
    // pending on this bus and calls its SignalTransferComplete().
    static void onTransferComplete(void* busHandle, bool failed);

protected:
    // The three actual hardware calls, supplied by a concrete
    // transport (Stm32HalI2CTransport, ArduinoWireTransport,
    // KR260I2cTransport, ...). Same non-blocking convention as the
    // public interface above: return true once the operation has
    // been *issued* — isBusy()/lastOpFailed() cover whether it
    // actually landed, separately.
    virtual bool halMemWrite(uint8_t reg, uint8_t* pData, uint16_t size) = 0;
    virtual bool halMemRead(uint8_t reg, uint8_t* pData, uint16_t size) = 0;
    virtual bool halIsDeviceReady(uint32_t trials, uint32_t timeout) = 0;

    // Acquires the bus mutex, waiting up to timeoutTicks. Default is
    // a no-op that reports success immediately ("nothing to obtain")
    // — this class still works correctly on a target with no real
    // mutex primitive, just without actual cross-task exclusion.
    // Override on a platform with a real RTOS — see
    // FreeRtosI2CTransport, which most concrete transports in this
    // codebase derive from instead of I2CTransport directly, so this
    // only needs implementing once.
    virtual bool ObtainMutex(uint32_t timeoutTicks) { (void)timeoutTicks; return true; }

    // Releases whatever ObtainMutex() acquired. Same no-op default —
    // if you override one of this pair, override both.
    virtual void ReleaseMutex() {}

    // Records that a transfer finished, for WaitForTransfer() below to
    // notice. Default just sets a plain flag (transferNotified_) —
    // correct on any target, RTOS or not, since WaitForTransfer()'s
    // default just polls that same flag. Override together with
    // WaitForTransfer() on a platform with a real notification
    // primitive (FreeRtosI2CTransport: osThreadFlagsSet()) to wake a
    // waiting task directly instead of it having to poll at all.
    virtual void SignalTransferComplete(bool failed);

    // Blocks until SignalTransferComplete() has been called since the
    // last check, up to timeoutTicks. Default busy-waits: checks the
    // flag, and if not yet set, calls YieldTick() before checking
    // again — timeoutTicks times at most. Sets failed_ as a side
    // effect once resolved either way this pair is implemented.
    virtual bool WaitForTransfer(uint32_t timeoutTicks);

    // Called between polls in the default WaitForTransfer() above.
    // No-op by default (a tight busy-spin) — override to actually
    // yield/delay for a bit instead, e.g. Arduino's delay(1), so a
    // target with no RTOS notification doesn't spin the CPU while
    // waiting. FreeRtosI2CTransport doesn't need this at all, since
    // it overrides WaitForTransfer() itself with a real blocking wait.
    virtual void YieldTick() {}

    void*   busHandle_;
    void*   busMutex_;    // opaque — see ObtainMutex()/ReleaseMutex() above
    uint8_t devAddr8bit_; // 7-bit address pre-shifted for HAL's 8-bit convention
    bool    failed_ = false; // set by WaitForTransfer() (default or overridden)

private:
    static constexpr uint32_t kMutexTimeoutTicks    = 100; // bus-contention bound
    static constexpr uint32_t kTransferTimeoutTicks = 50;  // single-transfer bound;
                                                              // meaning depends on the
                                                              // WaitForTransfer() override
                                                              // in use (RTOS ticks, or
                                                              // busy-wait poll iterations)

    uint8_t writeByte_ = 0; // must outlive the IT transfer
    bool    busy_ = false;
    bool    transferNotified_       = false; // set by the default SignalTransferComplete()
    bool    transferNotifiedFailed_ = false;

    static void registerActive(void* busHandle, I2CTransport* instance);
    static void unregisterActive(void* busHandle);
};
