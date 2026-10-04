#pragma once
#include "stm32f4xx_hal.h"
#include "Safe.h"
#include "Stm32HalUartTransport.h"
#include "UartLoopbackSafe.h"

// STM32-specific realization of a safety relay: two independent UART
// loopback channels, each using one of this MCU's own UART
// peripherals, combined into a single Safe implementation. This is
// the concrete STM32 case described at the start of this design — an
// STM32 acting as a safety relay by looping a known serial number
// back on two independent UARTs.
//
// Deliberately NOT reimplementing the loopback verification logic —
// GetSafe1State()/GetSafe2State()/SetSafe1Callback()/SetSafe2Callback()/
// poll() all forward straight to an internal UartLoopbackSafe, which
// already owns that logic and has already been tested against it
// directly. This class's only job is supplying the two STM32-specific
// Stm32HalUartTransport instances UartLoopbackSafe needs, so a caller
// doesn't have to wire up three separate objects (two transports plus
// the loopback checker) by hand for the common STM32 case.
class Stm32_Safe_Relay : public Safe {
public:
    // huart1/huart2 must already be configured (baud rate matching
    // whatever's on the other end of each loopback) via CubeMX before
    // this is constructed — same requirement Stm32HalUartTransport
    // itself has. serialNumber/serialNumberLen/timeoutTicks are
    // forwarded directly to the internal UartLoopbackSafe — see its
    // own header for what they mean.
    Stm32_Safe_Relay(UART_HandleTypeDef* huart1, UART_HandleTypeDef* huart2,
                      const uint8_t* serialNumber, size_t serialNumberLen,
                      uint32_t timeoutTicks);

    bool GetSafe1State() const override { return loopback_.GetSafe1State(); }
    bool GetSafe2State() const override { return loopback_.GetSafe2State(); }

    void SetSafe1Callback(SafeCallback callback, void* context = nullptr) override {
        loopback_.SetSafe1Callback(callback, context);
    }
    void SetSafe2Callback(SafeCallback callback, void* context = nullptr) override {
        loopback_.SetSafe2Callback(callback, context);
    }

    // Forwards to the internal UartLoopbackSafe's own poll() — call
    // periodically with the current tick count, same convention as
    // everywhere else in this codebase (e.g. HAL_GetTick()).
    void poll(uint32_t nowTicks) { loopback_.poll(nowTicks); }

private:
    // Declaration order matters here: transport1_/transport2_ must be
    // fully constructed before loopback_'s own constructor runs (it
    // calls setRxSink() on each of them) — C++ initializes members in
    // declaration order regardless of the constructor's own
    // initializer-list order, so these two must stay listed before
    // loopback_ below.
    Stm32HalUartTransport transport1_;
    Stm32HalUartTransport transport2_;
    UartLoopbackSafe      loopback_;
};
