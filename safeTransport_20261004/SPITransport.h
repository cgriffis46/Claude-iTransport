#pragma once
#include "ISensorTransport.h"

// Generic SPI transport: owns the tx/rx scratch buffers, the
// read/write framing (kReadBit convention, dummy clock bytes to drive
// full-duplex reads), and the busy/failed bookkeeping ISensorTransport
// requires — all hardware-call-agnostic.
//
// Every literal STM32 call (HAL_SPI_Transmit_IT, HAL_SPI_TransmitReceive_IT,
// HAL_SPI_GetState, HAL_SPI_GetError, and HAL_GPIO_WritePin for chip-select)
// lives in Stm32HalSPITransport now, and so does stm32f4xx_hal.h
// itself — this header has no STM32 type in it anywhere. That's
// deliberate, not just tidiness: many STM32Cube HAL versions typedef
// SPI_HandleTypeDef/GPIO_TypeDef from ANONYMOUS structs (no tag
// name), so forward-declaring them here would conflict with the real
// definition once Stm32HalSPITransport.h includes it — a genuine
// redefinition error, not a style concern. Storing the SPI handle and
// CS pin as opaque void*/uint32_t avoids that: this class never
// dereferences them, only passes them through to the hal*() methods,
// where the cast back to the real types happens.
class SPITransport : public ISensorTransport {
public:
    // spiHandle should be the bus's real HAL handle (e.g. &hspi1) and
    // csPort the real GPIO port (e.g. GPIOB), both passed through as
    // void* — Stm32HalSPITransport casts them back.
    SPITransport(void* spiHandle, void* csPort, uint16_t csPin);
    ~SPITransport() override = default;

    bool writeReg(uint8_t reg, uint8_t value) override;
    bool readRegs(uint8_t reg, uint8_t* buf, uint8_t len) override;
    bool isBusy() const override;
    bool lastOpFailed() const override;

    // SPI has no ACK-style bus signal the way I2C does, so there's no
    // real equivalent to HAL_I2C_IsDeviceReady() here. This always
    // returns true — actual presence-checking for an SPI sensor has
    // to happen at the sensor level (e.g. read a chip-ID register and
    // compare against the datasheet's expected value).
    bool checkDevice() override;

protected:
    // The actual hardware calls, supplied by Stm32HalSPITransport (or
    // any other concrete transport for a different HAL/platform).
    // Same non-blocking convention as the public interface above:
    // halTransmit()/halTransmitReceive() return true once *issued*;
    // halIsReady()/halLastTransferFailed() are consulted afterward,
    // from isBusy().
    virtual bool halTransmit(uint8_t* txBuf, uint16_t len) = 0;
    virtual bool halTransmitReceive(uint8_t* txBuf, uint8_t* rxBuf, uint16_t len) = 0;
    virtual bool halIsReady() const = 0;
    virtual bool halLastTransferFailed() const = 0;
    virtual void halCsLow() = 0;
    virtual void halCsHigh() = 0;

    void*    spiHandle_;
    void*    csPort_;
    uint16_t csPin_;

private:
    static constexpr uint8_t kReadBit = 0x80;  // BMP280/most Bosch/ST sensors: MSB set = read
    static constexpr uint8_t kMaxLen  = 33;    // 1 addr byte + up to 32 data bytes

    uint8_t  txBuf_[kMaxLen] = {0};
    uint8_t  rxBuf_[kMaxLen] = {0};
    uint8_t  xferLen_ = 0;
    uint8_t* destBuf_ = nullptr; // caller's buffer, filled in isBusy() once a read completes
    bool     isRead_  = false;
    bool     busy_    = false;
    bool     failed_  = false;
};
