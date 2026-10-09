#pragma once
#include <cstdint>
#include "hardware/i2c.h"
#include "pico/mutex.h"
#include "I2CTransport.h"
#include "PicoSyncTransport.h"

// I2C on the RP2040, through the Pico SDK, without blocking.
//
//   i2c_init(i2c0, 400 * 1000);                  // and the pins, as usual
//   gpio_set_function(4, GPIO_FUNC_I2C);
//   gpio_set_function(5, GPIO_FUNC_I2C);
//   static PicoI2CTransport bus(i2c0, 0x44);     // or (i2c0, 0x44, &i2c0Mutex)
//
// The SDK's own i2c_write/read functions wait for the bus; these don't.
// Each transfer is queued into the controller's 16 entry FIFO from the
// I2C interrupt, which also collects the bytes read, notices a NACK,
// and reports the transfer finished when the controller sends STOP.
// A register read is the register address, a repeated start and the
// reads, as one transaction.
//
// The interrupt handler is installed for this I2C block by the first
// transport constructed on it (as a shared handler, so it coexists
// with others). The block itself need not be initialised yet: nothing
// touches it until the first transfer. Use one I2C block either from
// these transports or from the SDK's blocking calls, not both while a
// transfer is in flight.
//
// checkDevice() is the one call that blocks, briefly: it reads one
// byte from the address with the SDK's i2c_read_timeout_us(), the
// usual way to probe on the RP2040 (the controller can't send an
// address with no data).
class PicoI2CTransport : public PicoSyncTransport<I2CTransport> {
public:
    // busMutex: a mutex_t shared by every transport on this I2C block,
    // or nullptr when only one core uses it. See PicoSyncTransport.
    PicoI2CTransport(i2c_inst_t* i2c, uint8_t deviceAddr7bit, mutex_t* busMutex = nullptr);

    // The I2C interrupt, for the block with index `index` (0 or 1).
    // Installed by the constructor; public for a test to call.
    static void handleIrq(unsigned index);

protected:
    bool halMemWrite(uint8_t reg, uint8_t* pData, uint16_t size) override;
    bool halMemRead(uint8_t reg, uint8_t* pData, uint16_t size) override;
    bool halMasterTransmit(uint8_t* pData, uint16_t size) override;
    bool halMasterReceive(uint8_t* pData, uint16_t size) override;
    bool halIsDeviceReady(uint32_t trials, uint32_t timeout) override;

private:
    i2c_inst_t* i2c() const { return static_cast<i2c_inst_t*>(busHandle_); }

    // Queues one transaction: an optional register byte, then `writeLen`
    // bytes from `writeData`, then `readLen` reads into `readData`.
    bool start(bool hasReg, uint8_t reg, const uint8_t* writeData, uint16_t writeLen,
               uint8_t* readData, uint16_t readLen);
};
