#include "PicoSPITransport.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"

namespace {

// Which transport each DMA channel's completion belongs to, by the
// channel's number. Only receive channels are entered.
constexpr unsigned kChannels = 12;
PicoSPITransport* s_byRxChannel[kChannels] = {};
bool s_irqInstalled[2] = {false, false};

void dmaIrq0() { PicoSPITransport::handleDmaIrq(0); }
void dmaIrq1() { PicoSPITransport::handleDmaIrq(1); }

} // namespace

PicoSPITransport::PicoSPITransport(spi_inst_t* spi, unsigned csPin, mutex_t* busMutex, unsigned dmaIrq)
    : PicoSyncTransport<SPITransport>(spi, nullptr, static_cast<uint16_t>(csPin), busMutex),
      dmaIrq_(dmaIrq ? 1 : 0) {
    gpio_init(csPin);
    gpio_put(csPin, true);
    gpio_set_dir(csPin, GPIO_OUT);

    txChan_ = dma_claim_unused_channel(false);
    rxChan_ = dma_claim_unused_channel(false);
    if (txChan_ < 0 || rxChan_ < 0) {   // no channels left: every transfer will be refused
        if (txChan_ >= 0) dma_channel_unclaim(static_cast<unsigned>(txChan_));
        if (rxChan_ >= 0) dma_channel_unclaim(static_cast<unsigned>(rxChan_));
        txChan_ = rxChan_ = -1;
        return;
    }

    s_byRxChannel[rxChan_] = this;
    dma_irqn_set_channel_enabled(dmaIrq_, static_cast<unsigned>(rxChan_), true);
    if (!s_irqInstalled[dmaIrq_]) {
        const unsigned irq = DMA_IRQ_0 + dmaIrq_;
        irq_add_shared_handler(irq, dmaIrq_ == 0 ? dmaIrq0 : dmaIrq1, PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY);
        irq_set_enabled(irq, true);
        s_irqInstalled[dmaIrq_] = true;
    }
}

PicoSPITransport::~PicoSPITransport() {
    if (rxChan_ < 0) return;
    dma_irqn_set_channel_enabled(dmaIrq_, static_cast<unsigned>(rxChan_), false);
    dma_channel_abort(static_cast<unsigned>(txChan_));
    dma_channel_abort(static_cast<unsigned>(rxChan_));
    s_byRxChannel[rxChan_] = nullptr;
    dma_channel_unclaim(static_cast<unsigned>(txChan_));
    dma_channel_unclaim(static_cast<unsigned>(rxChan_));
}

bool PicoSPITransport::start(const uint8_t* tx, uint8_t* rx, uint16_t len) {
    if (rxChan_ < 0 || len == 0) return false;
    spi_hw_t* hw = spi_get_hw(spi());

    // Anything left in the receive FIFO would land at the front of
    // this transfer's bytes, and a stale overrun would stop it.
    while (hw->sr & SPI_SSPSR_RNE_BITS) {
        const uint32_t d = hw->dr;
        (void)d;
    }
    hw->icr = SPI_SSPICR_RORIC_BITS;

    dma_channel_config c = dma_channel_get_default_config(static_cast<unsigned>(rxChan_));
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, rx != nullptr);
    channel_config_set_dreq(&c, spi_get_dreq(spi(), false));
    dma_channel_configure(static_cast<unsigned>(rxChan_), &c,
                          rx != nullptr ? rx : &rxScratch_, &hw->dr, len, false);

    c = dma_channel_get_default_config(static_cast<unsigned>(txChan_));
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, spi_get_dreq(spi(), true));
    dma_channel_configure(static_cast<unsigned>(txChan_), &c, &hw->dr, tx, len, false);

    // Both at once, so the receive side is ready for the first byte.
    dma_start_channel_mask((1u << rxChan_) | (1u << txChan_));
    return true;
}

// Interrupt context. The receive channel finishing means every byte
// has been clocked out and in.
void PicoSPITransport::handleDmaIrq(unsigned dmaIrq) {
    for (unsigned ch = 0; ch < kChannels; ++ch) {
        PicoSPITransport* t = s_byRxChannel[ch];
        if (t == nullptr || t->dmaIrq_ != dmaIrq) continue;
        if (!dma_irqn_get_channel_status(dmaIrq, ch)) continue;
        dma_irqn_acknowledge_channel(dmaIrq, ch);
        BusTransport::onTransferComplete(t->spi(), false);
    }
}

bool PicoSPITransport::halTransmit(uint8_t* txBuf, uint16_t len) {
    return start(txBuf, nullptr, len);
}

bool PicoSPITransport::halTransmitReceive(uint8_t* txBuf, uint8_t* rxBuf, uint16_t len) {
    return start(txBuf, rxBuf, len);
}

void PicoSPITransport::halCsLow() {
    gpio_put(csPin_, false);
}

void PicoSPITransport::halCsHigh() {
    gpio_put(csPin_, true);
}
