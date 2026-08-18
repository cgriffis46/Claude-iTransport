#include "Stm32HalSPITransport.h"

void Stm32HalSPITransport::halCsLow()  { HAL_GPIO_WritePin(csGpio(), csPin_, GPIO_PIN_RESET); }
void Stm32HalSPITransport::halCsHigh() { HAL_GPIO_WritePin(csGpio(), csPin_, GPIO_PIN_SET); }

bool Stm32HalSPITransport::halTransmit(uint8_t* txBuf, uint16_t len) {
    return HAL_SPI_Transmit_IT(hspi(), txBuf, len) == HAL_OK;
}

bool Stm32HalSPITransport::halTransmitReceive(uint8_t* txBuf, uint8_t* rxBuf, uint16_t len) {
    return HAL_SPI_TransmitReceive_IT(hspi(), txBuf, rxBuf, len) == HAL_OK;
}

bool Stm32HalSPITransport::halIsReady() const {
    return HAL_SPI_GetState(hspi()) == HAL_SPI_STATE_READY;
}

bool Stm32HalSPITransport::halLastTransferFailed() const {
    return HAL_SPI_GetError(hspi()) != HAL_SPI_ERROR_NONE;
}
