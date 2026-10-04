#pragma once
// Includes the project's own main.h rather than hardcoding a specific
// family's _hal.h (e.g. stm32f4xx_hal.h) -- confirmed against the
// real STM32F207ZG_SafeRelay project that CubeMX's own generated
// main.h already includes the CORRECT family header for whichever
// target a given project actually is (stm32f2xx_hal.h there). This
// one change makes this file portable across any STM32 family project
// without modification, rather than silently assuming F4 the way an
// earlier version of this file did -- a real, confirmed mismatch when
// checked against this specific F2-series project.
#include "main.h"
#include "iTransport.h"

// STM32 HAL implementation of iTransport. NO RTOS dependency at
// all anymore — that's deliberate, and the whole point of the
// iTransportRxSink redesign: this class only ever calls rxSink_.onByteReceived()
// from its ISR and has no idea what happens after that. Whichever
// concrete sensor uses this (xBNO085, say) owns the actual buffering
// mechanism (a FreeRTOS stream buffer, for instance) — this class
// doesn't include cmsis_os2.h, FreeRTOS.h, or anything RTOS-specific.
//
// TX: HAL_UART_Transmit_IT() — interrupt-driven, non-blocking. Same
// busy-tracking convention as everywhere else in this codebase: won't
// issue a new send while a previous one is still in flight.
//
// RX: byte-at-a-time via HAL_UART_Receive_IT(huart, &rxByte_, 1),
// re-armed immediately after every single byte in the ISR callback —
// the standard way to handle a variable-length, delimiter-framed
// protocol (like BNO085's SHTP-over-UART) when the frame length isn't
// known in advance. Each received byte is handed to rxSink_ directly,
// synchronously, from ISR context.
class Stm32HalUartTransport : public iTransport {
public:
    // huart must already be configured (baud rate, etc.) via CubeMX/
    // MX_USARTx_Init() before this is constructed. No sink is required
    // here — attach one afterward via setRxSink(); until then,
    // received bytes are silently dropped (see setRxSink()'s own
    // comment in iTransport.h for why this shape exists).
    explicit Stm32HalUartTransport(UART_HandleTypeDef* huart);

    bool write(const uint8_t* data, size_t len) override;
    void setRxSink(iTransportRxSink& sink) override { rxSink_ = &sink; }

    // Call these from the project's HAL_UART_TxCpltCallback and
    // HAL_UART_RxCpltCallback for `huart` — ISR context. Finds the
    // instance registered against this huart and dispatches to it.
    static void onTxComplete(UART_HandleTypeDef* huart);
    static void onRxComplete(UART_HandleTypeDef* huart);

private:
    void handleTxComplete();
    void handleRxComplete();
    void armNextByteReceive(); // re-issues HAL_UART_Receive_IT for the next single byte

    UART_HandleTypeDef* huart_;
    iTransportRxSink*         rxSink_ = nullptr; // nullptr until setRxSink() is called
    uint8_t              rxByte_ = 0; // must outlive each single-byte IT receive
    bool                 txBusy_ = false;

    static void registerActive(UART_HandleTypeDef* huart, Stm32HalUartTransport* instance);
    static void unregisterActive(UART_HandleTypeDef* huart);
};
