#include "Stm32HalI2CTransport.h"

bool Stm32HalI2CTransport::halMemWrite(uint8_t reg, uint8_t* pData, uint16_t size) {
    return HAL_I2C_Mem_Write_IT(hi2c(), devAddr8bit_, reg, I2C_MEMADD_SIZE_8BIT, pData, size) == HAL_OK;
}

bool Stm32HalI2CTransport::halMemRead(uint8_t reg, uint8_t* pData, uint16_t size) {
    return HAL_I2C_Mem_Read_IT(hi2c(), devAddr8bit_, reg, I2C_MEMADD_SIZE_8BIT, pData, size) == HAL_OK;
}

// The one HAL call in this whole codebase with no _IT/_DMA variant —
// genuinely blocking, bounded by `timeout` per trial. See
// I2CTransport::checkDevice() for why that's still acceptable here
// (one-time check, not part of the steady-state measurement cycle).
bool Stm32HalI2CTransport::halIsDeviceReady(uint32_t trials, uint32_t timeout) {
    return HAL_I2C_IsDeviceReady(hi2c(), devAddr8bit_, trials, timeout) == HAL_OK;
}
