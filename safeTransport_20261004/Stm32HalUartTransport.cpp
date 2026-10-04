#include "Stm32HalUartTransport.h"
#include <cstddef>

namespace {
    // Same small fixed registry pattern as I2CTransport's
    // registerActive/onTransferComplete — HAL_UART_TxCpltCallback/
    // HAL_UART_RxCpltCallback are global weak callbacks keyed by
    // UART_HandleTypeDef*, not per-instance, so this is what maps a
    // given huart back to the Stm32HalUartTransport instance waiting
    // on it. Bump kMaxUarts if a project uses more than 4 UARTs this way.
    constexpr size_t kMaxUarts = 4;
    UART_HandleTypeDef*    s_uartHandle[kMaxUarts]   = {nullptr, nullptr, nullptr, nullptr};
    Stm32HalUartTransport* s_activeOnUart[kMaxUarts] = {nullptr, nullptr, nullptr, nullptr};
}

void Stm32HalUartTransport::registerActive(UART_HandleTypeDef* huart, Stm32HalUartTransport* instance) {
    for (size_t i = 0; i < kMaxUarts; ++i) {
        if (s_uartHandle[i] == nullptr || s_uartHandle[i] == huart) {
            s_uartHandle[i]   = huart;
            s_activeOnUart[i] = instance;
            return;
        }
    }
    // registry full — bump kMaxUarts if this project uses more UARTs
}

void Stm32HalUartTransport::unregisterActive(UART_HandleTypeDef* huart) {
    for (size_t i = 0; i < kMaxUarts; ++i) {
        if (s_uartHandle[i] == huart) {
            s_activeOnUart[i] = nullptr;
            return;
        }
    }
}

void Stm32HalUartTransport::onTxComplete(UART_HandleTypeDef* huart) {
    for (size_t i = 0; i < kMaxUarts; ++i) {
        if (s_uartHandle[i] == huart && s_activeOnUart[i] != nullptr) {
            s_activeOnUart[i]->handleTxComplete();
            return;
        }
    }
}

void Stm32HalUartTransport::onRxComplete(UART_HandleTypeDef* huart) {
    for (size_t i = 0; i < kMaxUarts; ++i) {
        if (s_uartHandle[i] == huart && s_activeOnUart[i] != nullptr) {
            s_activeOnUart[i]->handleRxComplete();
            return;
        }
    }
}

Stm32HalUartTransport::Stm32HalUartTransport(UART_HandleTypeDef* huart)
    : huart_(huart) {
    registerActive(huart_, this);
    armNextByteReceive(); // start listening immediately — SHTP data can arrive unsolicited
}

void Stm32HalUartTransport::armNextByteReceive() {
    HAL_UART_Receive_IT(huart_, &rxByte_, 1);
}

bool Stm32HalUartTransport::write(const uint8_t* data, size_t len) {
    if (txBusy_) return false; // previous send still in flight

    const HAL_StatusTypeDef st = HAL_UART_Transmit_IT(
        huart_, const_cast<uint8_t*>(data), static_cast<uint16_t>(len));
    txBusy_ = (st == HAL_OK);
    return txBusy_;
}

// ISR context.
void Stm32HalUartTransport::handleTxComplete() {
    txBusy_ = false;
}

// ISR context. Hands the just-received byte directly to rxSink_ if
// one's been attached — this class has no idea what the sink does
// with it (a FreeRTOS stream buffer, in xBNO085's case). If no sink
// has been attached yet, the byte is silently dropped — see
// setRxSink()'s own comment for why that's the correct default rather
// than an error. Either way, immediately re-arms the next single-byte
// receive so nothing is missed while this runs.
void Stm32HalUartTransport::handleRxComplete() {
    if (rxSink_) rxSink_->onByteReceived(rxByte_);
    armNextByteReceive();
}
