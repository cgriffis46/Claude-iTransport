#pragma once
// The project's own main.h, not a specific family's _hal.h — see
// Stm32HalI2CTransport.h. The HAL header lives here, not in
// SPITransport.h, for the anonymous-struct reason given there.
#include "main.h"
#include "SPITransport.h"
#include "FreeRtosTransport.h"

// The literal STM32 HAL_SPI_*/HAL_GPIO_* calls — the interrupt-driven
// (_IT) variants, completing through Stm32SpiItCallbacks.cpp —
// isolated here so SPITransport's generic framing logic doesn't
// depend on which specific HAL function signature a given STM32
// family or HAL version uses. This is the class you actually
// construct — SPITransport itself is abstract. Derives from
// FreeRtosTransport<SPITransport> (not SPITransport directly) to get
// a real bus mutex and to be woken by the SPI interrupt rather than
// polling, exactly as Stm32HalI2CTransport does for I2C.
//
// The SPI global interrupt must be enabled for the peripheral in
// CubeMX. busMutex: one per SPI bus, shared by every device on it,
// created with osMutexNew() before constructing this.
class Stm32HalSPITransport : public FreeRtosTransport<SPITransport> {
public:
    Stm32HalSPITransport(SPI_HandleTypeDef* hspi, GPIO_TypeDef* csPort, uint16_t csPin, osMutexId_t busMutex)
        : FreeRtosTransport<SPITransport>(hspi, csPort, csPin, busMutex) {
        halCsHigh(); // idle high — deselected. Safe here specifically because
                      // we're in this class's own constructor, not SPITransport's.
    }

protected:
    bool halTransmit(uint8_t* txBuf, uint16_t len) override;
    bool halTransmitReceive(uint8_t* txBuf, uint8_t* rxBuf, uint16_t len) override;
    void halCsLow() override;
    void halCsHigh() override;

private:
    // busHandle_/csPort_ (inherited, protected, void*) always actually
    // hold an SPI_HandleTypeDef*/GPIO_TypeDef* here — this is the one
    // place that's true, since only this class constructs an
    // SPITransport with real STM32 handles to begin with.
    SPI_HandleTypeDef* hspi() const { return static_cast<SPI_HandleTypeDef*>(busHandle_); }
    GPIO_TypeDef*      csGpio() const { return static_cast<GPIO_TypeDef*>(csPort_); }
};
