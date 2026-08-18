#pragma once
#include "stm32f4xx_hal.h"   // moved here from I2CTransport.h — see the comment
                              // there on why an anonymous-struct HAL typedef made
                              // that unsafe to forward-declare in the base class.
#include "FreeRtosI2CTransport.h"

// The three literal STM32 HAL_I2C_* calls, isolated here so
// I2CTransport's generic mutex/notification/bookkeeping logic doesn't
// depend on which specific HAL function signature a given STM32
// family or HAL version uses. This is the class you actually
// construct — I2CTransport itself is abstract. Derives from
// FreeRtosI2CTransport (not I2CTransport directly) to get real
// osMutexAcquire()/osMutexRelease() calls for free.
class Stm32HalI2CTransport : public FreeRtosI2CTransport {
public:
    Stm32HalI2CTransport(I2C_HandleTypeDef* hi2c, uint8_t deviceAddr7bit, osMutexId_t busMutex)
        : FreeRtosI2CTransport(hi2c, deviceAddr7bit, busMutex) {}

protected:
    bool halMemWrite(uint8_t reg, uint8_t* pData, uint16_t size) override;
    bool halMemRead(uint8_t reg, uint8_t* pData, uint16_t size) override;
    bool halIsDeviceReady(uint32_t trials, uint32_t timeout) override;

private:
    // busHandle_ (inherited, protected, void*) always actually holds
    // an I2C_HandleTypeDef* here — this is the one place that's true
    // and the one place it's safe to assume, since only this class
    // constructs an I2CTransport with an I2C_HandleTypeDef* to begin with.
    I2C_HandleTypeDef* hi2c() const { return static_cast<I2C_HandleTypeDef*>(busHandle_); }
};
