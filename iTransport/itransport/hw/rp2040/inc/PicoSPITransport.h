#pragma once
#include <cstdint>
#include "hardware/spi.h"
#include "pico/mutex.h"
#include "SPITransport.h"
#include "PicoSyncTransport.h"

// SPI on the RP2040, through the Pico SDK, without blocking.
//
//   spi_init(spi0, 1000 * 1000);                    // mode, pins, as usual
//   spi_set_format(spi0, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
//   gpio_set_function(18, GPIO_FUNC_SPI);           // SCK, MOSI, MISO
//   ...
//   static PicoSPITransport bus(spi0, 17);          // chip-select on GPIO 17
//
// Each transfer runs on two DMA channels, one feeding the SPI transmit
// FIFO and one emptying the receive FIFO (into a scratch byte for a
// write), and finishes in the receive channel's interrupt: by then
// every byte has been clocked both ways. The channels are claimed by
// the constructor, and given back by the destructor.
//
// Chip-select is a plain GPIO, driven here: low before the transfer,
// high once it has landed. The constructor sets the pin up as an
// output, high.
//
// The DMA interrupt (DMA_IRQ_0 by default; DMA_IRQ_1, or on the RP2350
// up to DMA_IRQ_3) is installed as a shared handler by the first
// transport constructed on it, so it coexists with other DMA users of
// the same line. Works on the RP2040 (12 DMA channels) and the RP2350
// (16).
class PicoSPITransport : public PicoSyncTransport<SPITransport> {
public:
    // busMutex: a mutex_t shared by every transport on this SPI block,
    // or nullptr when only one core uses it. See PicoSyncTransport.
    // dmaIrq: which DMA interrupt line the channels use: 0 or 1, and on
    // the RP2350 also 2 or 3. Anything else means 0.
    PicoSPITransport(spi_inst_t* spi, unsigned csPin, mutex_t* busMutex = nullptr, unsigned dmaIrq = 0);
    ~PicoSPITransport() override;

    // The DMA interrupt for line `dmaIrq`. Installed by the
    // constructor; public for a test to call.
    static void handleDmaIrq(unsigned dmaIrq);

protected:
    bool halTransmit(uint8_t* txBuf, uint16_t len) override;
    bool halTransmitReceive(uint8_t* txBuf, uint8_t* rxBuf, uint16_t len) override;
    void halCsLow() override;
    void halCsHigh() override;

private:
    spi_inst_t* spi() const { return static_cast<spi_inst_t*>(busHandle_); }
    bool start(const uint8_t* tx, uint8_t* rx, uint16_t len);

    int      txChan_ = -1;
    int      rxChan_ = -1;
    unsigned dmaIrq_;
    uint8_t  rxScratch_ = 0;   // where a write's received bytes go
};
