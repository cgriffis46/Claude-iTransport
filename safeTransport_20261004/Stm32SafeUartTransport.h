#pragma once
#include "stm32f4xx_hal.h"
#include "SafeTransport.h"
#include "Stm32HalUartTransport.h"
#include "UartLoopbackSafe.h"

// Concrete SafeTransport<iTransport>: three physically independent
// UART peripherals doing two different jobs.
//
//   huart1, huart2 — dedicated loopback channels, exactly as in
//   Stm32_Safe_Relay/UartLoopbackSafe: each transmits a known serial
//   number and confirms it loops back correctly, continuously
//   re-verified via poll(). This drives Safe1/Safe2.
//
//   huartData — a THIRD, separate UART, carrying actual data (e.g. a
//   UART-to-CAN bridge for safety-relevant CAN traffic) — exposed via
//   this class's own write()/setRxSink(), the iTransport half of
//   SafeTransport<iTransport>.
//
// These three channels are deliberately never mixed: the loopback
// channels only ever see the fixed serial number pattern go out and
// come back; huartData only ever carries real application data. A
// single shared channel doing both would corrupt the loopback state
// machine's exact-echo expectation the moment real data arrived on
// it, and vice versa — that's why this needs three UARTs, not one or
// two, for a design that does both jobs.
//
// Safe1/Safe2 reporting is simply forwarded to an internal
// UartLoopbackSafe — no new verification logic here, same as
// Stm32_Safe_Relay. write()/setRxSink() are simply forwarded to the
// third transport — no new transport logic here either. This class's
// only real job is wiring three independent channels together under
// one SafeTransport<iTransport> object.
class Stm32SafeUartTransport : public SafeTransport<iTransport> {
public:
    // huart1/huart2/huartData must all already be configured via
    // CubeMX before this is constructed (same requirement
    // Stm32HalUartTransport itself has). serialNumber/serialNumberLen/
    // timeoutTicks are forwarded to the internal UartLoopbackSafe —
    // see its own header for what they mean.
    Stm32SafeUartTransport(UART_HandleTypeDef* huart1, UART_HandleTypeDef* huart2,
                            UART_HandleTypeDef* huartData,
                            const uint8_t* serialNumber, size_t serialNumberLen,
                            uint32_t timeoutTicks);

    // Safe — forwards to the internal loopback checker.
    bool GetSafe1State() const override { return loopback_.GetSafe1State(); }
    bool GetSafe2State() const override { return loopback_.GetSafe2State(); }
    void SetSafe1Callback(SafeCallback callback, void* context = nullptr) override {
        loopback_.SetSafe1Callback(callback, context);
    }
    void SetSafe2Callback(SafeCallback callback, void* context = nullptr) override {
        loopback_.SetSafe2Callback(callback, context);
    }

    // iTransport — forwards to the third, independent data UART.
    bool write(const uint8_t* data, size_t len) override { return dataTransport_.write(data, len); }
    void setRxSink(iTransportRxSink& sink) override { dataTransport_.setRxSink(sink); }

    // Forwards to the internal UartLoopbackSafe's own poll() — call
    // periodically with the current tick count, same as
    // Stm32_Safe_Relay. Only drives the loopback channels; huartData
    // needs no polling of its own (its ISR-driven receive already
    // runs independently, same as any other Stm32HalUartTransport).
    void poll(uint32_t nowTicks) { loopback_.poll(nowTicks); }

private:
    // Declaration order matters: loopbackTransport1_/loopbackTransport2_
    // must be fully constructed before loopback_'s own constructor
    // runs (it calls setRxSink() on each of them) — same reasoning as
    // Stm32_Safe_Relay. dataTransport_ has no such dependency.
    Stm32HalUartTransport loopbackTransport1_;
    Stm32HalUartTransport loopbackTransport2_;
    Stm32HalUartTransport dataTransport_;
    UartLoopbackSafe      loopback_;
};
