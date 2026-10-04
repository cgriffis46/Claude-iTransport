#include "Stm32HalUartTransport.h"

// STM32 HAL calls these (weak, overridable) from the UART interrupt
// handler once a Transmit_IT/Receive_IT transfer finishes. Same
// reasoning as Stm32I2CItCallbacks.cpp: HAL callbacks are free C
// functions per peripheral, not per-instance, so
// Stm32HalUartTransport::onTxComplete()/onRxComplete() do the work of
// finding which instance was actually waiting.
extern "C" {

void HAL_UART_TxCpltCallback(UART_HandleTypeDef* huart) {
    Stm32HalUartTransport::onTxComplete(huart);
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef* huart) {
    Stm32HalUartTransport::onRxComplete(huart);
}

} // extern "C"
