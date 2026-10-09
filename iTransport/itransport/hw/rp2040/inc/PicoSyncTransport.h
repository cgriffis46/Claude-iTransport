#pragma once
#include <cstdint>
#include <type_traits>
#include "pico/mutex.h"
#include "BusTransport.h"

// Bus arbitration for the RP2040 without an RTOS, using the Pico SDK's
// mutex_t. PicoI2CTransport and PicoSPITransport derive from this.
//
// busMutex is a mutex_t* — the same one for every transport on the
// same physical bus — or nullptr. With nullptr nothing is locked,
// which is all a program needs when one core uses the bus:
// beginTransfer() already refuses a transfer while another device's
// is in flight on the same bus. Give a mutex when both cores use it.
//
// The mutex is only ever tried, never waited for. A driver whose bus
// is taken gets false back from writeReg()/readRegs() and tries again
// on its next pass, as it does for any other busy bus, so main() still
// never blocks. Released by isBusy() once the transfer lands, on the
// same core that took it.
//
// Under FreeRTOS with CMSIS-RTOS2, wrap the transport instead —
// FreeRtosTransport<PicoI2CTransport> — and pass an osMutexId_t: its
// ObtainMutex()/ReleaseMutex() replace these.
template <typename TBus>
class PicoSyncTransport : public TBus {
    static_assert(std::is_base_of<BusTransport, TBus>::value,
                  "PicoSyncTransport<TBus> requires TBus to derive from BusTransport.");

public:
    using TBus::TBus;

protected:
    bool ObtainMutex(uint32_t timeoutTicks) override {
        (void)timeoutTicks;
        if (this->busMutex_ == nullptr) return true;
        return mutex_try_enter(static_cast<mutex_t*>(this->busMutex_), nullptr);
    }

    void ReleaseMutex() override {
        if (this->busMutex_ != nullptr) mutex_exit(static_cast<mutex_t*>(this->busMutex_));
    }
};
