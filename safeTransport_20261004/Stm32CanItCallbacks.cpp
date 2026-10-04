#include "Stm32HalCanTransport.h"

// STM32 HAL calls this (weak, overridable) from the CAN interrupt
// handler once a message is pending in RX FIFO 0. Same reasoning as
// Stm32I2CItCallbacks.cpp/Stm32UartItCallbacks.cpp: HAL callbacks are
// free C functions per peripheral, not per-instance, so
// Stm32HalCanTransport::onRxFifo0MsgPending() does the work of
// finding which instance was actually waiting.
extern "C" {

void HAL_CAN_RxFifo0MsgPendingCallback(CAN_HandleTypeDef* hcan) {
    Stm32HalCanTransport::onRxFifo0MsgPending(hcan);
}

} // extern "C"
