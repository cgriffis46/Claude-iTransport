#include "stm32f4xx_hal.h" // I2CTransport.h no longer provides this — see its own comment
#include "I2CTransport.h"

// STM32 HAL calls these (weak, overridable) from the I2C interrupt
// handler once a Mem_Write_IT/Mem_Read_IT transfer finishes or faults.
// This is the one piece of glue code that has to live outside the
// class — HAL callbacks are free C functions per peripheral, not
// per-instance, so I2CTransport::onTransferComplete() does the work
// of finding which instance was actually waiting.
extern "C" {

void HAL_I2C_MemTxCpltCallback(I2C_HandleTypeDef* hi2c) {
    I2CTransport::onTransferComplete(hi2c, /*failed=*/false);
}

void HAL_I2C_MemRxCpltCallback(I2C_HandleTypeDef* hi2c) {
    I2CTransport::onTransferComplete(hi2c, /*failed=*/false);
}

void HAL_I2C_ErrorCallback(I2C_HandleTypeDef* hi2c) {
    I2CTransport::onTransferComplete(hi2c, /*failed=*/true);
}

} // extern "C"
