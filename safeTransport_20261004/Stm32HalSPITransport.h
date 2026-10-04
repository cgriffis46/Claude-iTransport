#pragma once
#include "stm32f4xx_hal.h"   // moved here from SPITransport.h — see the comment
                              // there on why an anonymous-struct HAL typedef made
                              // that unsafe to forward-declare in the base class.
#include "SPITransport.h"

// The literal STM32 HAL_SPI_*/HAL_GPIO_* calls, isolated here so
// SPITransport's generic framing/bookkeeping logic doesn't depend on
// which specific HAL function signature a given STM32 family or HAL
// version uses. This is the class you actually construct —
// SPITransport itself is abstract.
class Stm32HalSPITransport : public SPITransport {
public:
    Stm32HalSPITransport(SPI_HandleTypeDef* hspi, GPIO_TypeDef* csPort, uint16_t csPin)
        : SPITransport(hspi, csPort, csPin) {
        halCsHigh(); // idle high — deselected. Safe here specifically because
                      // we're in this class's own constructor, not SPITransport's.
    }

protected:
    bool halTransmit(uint8_t* txBuf, uint16_t len) override;
    bool halTransmitReceive(uint8_t* txBuf, uint8_t* rxBuf, uint16_t len) override;
    bool halIsReady() const override;
    bool halLastTransferFailed() const override;
    void halCsLow() override;
    void halCsHigh() override;

private:
    // spiHandle_/csPort_ (inherited, protected, void*) always actually
    // hold an SPI_HandleTypeDef*/GPIO_TypeDef* here — this is the one
    // place that's true, since only this class constructs an
    // SPITransport with real STM32 handles to begin with.
    SPI_HandleTypeDef* hspi() const { return static_cast<SPI_HandleTypeDef*>(spiHandle_); }
    GPIO_TypeDef*      csGpio() const { return static_cast<GPIO_TypeDef*>(csPort_); }
};
