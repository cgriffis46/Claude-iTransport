// The simulated RP2040 peripherals behind the stand-in Pico SDK
// headers. See pico_sim.h.
#include "pico_sim.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "hardware/irq.h"
#include "hardware/spi.h"
#include "hardware/uart.h"

i2c_hw_t   sim_i2c_hw[2];
i2c_inst_t i2c0_inst = {&sim_i2c_hw[0]};
i2c_inst_t i2c1_inst = {&sim_i2c_hw[1]};
spi_hw_t   sim_spi_hw[2];
uart_hw_t  sim_uart_hw[2];

namespace sim {

I2cBlock   i2c[2];
SpiBlock   spi[2];
UartBlock  uart[2];
DmaChannel dma[12];
bool       gpioOut[32];
int        gpioLevel[32];
int        dmaChannelsAvailable = 12;
unsigned   isrCalls = 0;
std::vector<irq_handler_t> handlers[32];
bool       irqEnabled[32];

// ---- I2C ----

uint32_t I2cBlock::raw() const {
    uint32_t r = 0;
    if (enabled && tx.size() <= tx_tl) r |= I2C_IC_INTR_STAT_R_TX_EMPTY_BITS;
    if (rx.size() > rx_tl)             r |= I2C_IC_INTR_STAT_R_RX_FULL_BITS;
    if (abrt)                          r |= I2C_IC_INTR_STAT_R_TX_ABRT_BITS;
    if (stopDet)                       r |= I2C_IC_INTR_STAT_R_STOP_DET_BITS;
    return r;
}

// One command off the transmit FIFO onto the wire, every bytePeriod
// ticks. A start (or repeated start) goes out first when needed: on
// the first command, on RESTART, or when the direction changes. No
// device at the address is a NACK: the controller flushes the FIFO,
// sets TX_ABRT, and sends STOP.
void I2cBlock::step() {
    if (!enabled || tx.empty()) return;
    if (++ticks < bytePeriod) return;
    ticks = 0;
    const uint32_t cmd = tx.front();
    tx.pop_front();
    const bool isRead = (cmd & I2C_IC_DATA_CMD_CMD_BITS) != 0;
    RegDevice* dev = devices[tar & 0x7F];
    if (!inTxn || (cmd & I2C_IC_DATA_CMD_RESTART_BITS) || isRead != lastWasRead) {
        if (dev == nullptr) {
            abrt = true;
            tx.clear();
            inTxn = false;
            stopDet = true;
            return;
        }
        dev->start(inTxn);
        inTxn = true;
    }
    lastWasRead = isRead;
    if (isRead) {
        if (rx.size() >= 16) rxOverflow = true;
        else rx.push_back(dev->read());
    } else {
        dev->write(static_cast<uint8_t>(cmd & 0xFF));
    }
    if (cmd & I2C_IC_DATA_CMD_STOP_BITS) {
        dev->stop();
        inTxn = false;
        stopDet = true;
    }
}

static void wireI2c(unsigned k) {
    i2c_hw_t& h = sim_i2c_hw[k];
    I2cBlock& b = i2c[k];
    h.enable.bind([&b] { return uint32_t(b.enabled); }, [&b](uint32_t v) { b.enabled = v & 1; });
    h.tar.bind([&b] { return b.tar; }, [&b](uint32_t v) { b.tar = v; });
    h.data_cmd.bind([&b] { uint32_t d = 0; if (!b.rx.empty()) { d = b.rx.front(); b.rx.pop_front(); } return d; },
                             [&b](uint32_t v) { if (b.abrt) return; if (b.tx.size() >= 16) { b.txOverflow = true; return; } b.tx.push_back(v); });
    h.intr_mask.bind([&b] { return b.mask; }, [&b](uint32_t v) { b.mask = v; });
    h.intr_stat.bind([&b] { return b.raw() & b.mask; }, nullptr);
    h.raw_intr_stat.bind([&b] { return b.raw(); }, nullptr);
    h.rx_tl.bind([&b] { return b.rx_tl; }, [&b](uint32_t v) { b.rx_tl = v; });
    h.tx_tl.bind([&b] { return b.tx_tl; }, [&b](uint32_t v) { b.tx_tl = v; });
    h.clr_intr.bind([&b] { b.abrt = false; b.stopDet = false; return 0u; }, nullptr);
    h.clr_tx_abrt.bind([&b] { b.abrt = false; return 0u; }, nullptr);
    h.clr_stop_det.bind([&b] { b.stopDet = false; return 0u; }, nullptr);
    h.txflr.bind([&b] { return uint32_t(b.tx.size()); }, nullptr);
    h.rxflr.bind([&b] { return uint32_t(b.rx.size()); }, nullptr);
}

// ---- SPI and UART registers ----

static void wireSpi(unsigned k) {
    SpiBlock& b = spi[k];
    sim_spi_hw[k].dr.bind([&b] { uint32_t d = 0; if (!b.rx.empty()) { d = b.rx.front(); b.rx.pop_front(); } return d; }, [](uint32_t) {});
    sim_spi_hw[k].sr.bind([&b] { return SPI_SSPSR_TNF_BITS | (b.rx.empty() ? 0u : SPI_SSPSR_RNE_BITS); }, nullptr);
    sim_spi_hw[k].icr.bind(nullptr, [&b](uint32_t v) { if (v & SPI_SSPICR_RORIC_BITS) b.overrunCleared = true; });
}

static void wireUart(unsigned k) {
    UartBlock& b = uart[k];
    sim_uart_hw[k].dr.bind([&b] { uint32_t d = 0; if (!b.rx.empty()) { d = b.rx.front(); b.rx.pop_front(); } return d; }, [](uint32_t) {});
    sim_uart_hw[k].fr.bind([&b] { return b.rx.empty() ? UART_UARTFR_RXFE_BITS : 0u; }, nullptr);
    sim_uart_hw[k].icr.bind(nullptr, [&b](uint32_t v) { if (v & UART_UARTICR_OEIC_BITS) b.overrun = false; });
}

void reset() {
    for (unsigned k = 0; k < 2; ++k) {
        i2c[k] = I2cBlock(); spi[k] = SpiBlock(); uart[k] = UartBlock();
        wireI2c(k); wireSpi(k); wireUart(k);
    }
    for (auto& c : dma) c = DmaChannel();
    for (int g = 0; g < 32; ++g) { gpioOut[g] = false; gpioLevel[g] = -1; }
    // Interrupt handlers stay installed, as they do on the chip for the
    // life of the program: the transports install theirs once per bus.
    dmaChannelsAvailable = 12;
    isrCalls = 0;
}

// ---- DMA ----

// An SPI transfer: the transmit channel into dr and the receive
// channel out of it, run together as one chip-select frame on whichever
// device's chip-select is low. Bytes already in the receive FIFO come
// out ahead of the new ones, as on the chip.
static void runSpi(unsigned k, DmaChannel& tx, DmaChannel& rx) {
    RegDevice* dev = nullptr;
    for (int g = 0; g < 32; ++g) if (spi[k].devices[g] && gpioLevel[g] == 0) dev = spi[k].devices[g];
    const uint8_t* src = (const uint8_t*)tx.read;
    std::deque<uint8_t> in(spi[k].rx.begin(), spi[k].rx.end());
    spi[k].rx.clear();
    bool isRead = false;
    if (dev) dev->start(false);
    for (uint32_t i = 0; i < tx.count; ++i) {
        const uint8_t out = tx.readInc ? src[i] : src[0];
        uint8_t back = 0xFF;
        if (dev) {
            if (i == 0) { isRead = out & 0x80; dev->ptr = out & 0x7F; dev->log.push_back("a" + std::to_string(out)); }
            else if (isRead) back = dev->read();
            else { dev->log.push_back("w" + std::to_string(out)); dev->reg[dev->ptr++] = out; }
        }
        in.push_back(back);
    }
    if (dev) dev->stop();
    uint8_t* dst = (uint8_t*)rx.write;
    for (uint32_t i = 0; i < rx.count; ++i) { dst[rx.writeInc ? i : 0] = in.front(); in.pop_front(); }
    tx.busy = rx.busy = false;
    for (int l = 0; l < 2; ++l) if (rx.irqLine[l]) rx.status[l] = true;
}

static void stepDma() {
    for (unsigned k = 0; k < 2; ++k) {
        DmaChannel* tx = nullptr; DmaChannel* rx = nullptr;
        for (auto& c : dma) {
            if (!c.busy) continue;
            if (c.write == (volatile void*)&sim_spi_hw[k].dr) tx = &c;
            if (c.read == (const volatile void*)&sim_spi_hw[k].dr) rx = &c;
            if (c.write == (volatile void*)&sim_uart_hw[k].dr) {
                const uint8_t* src = (const uint8_t*)c.read;
                for (uint32_t i = 0; i < c.count; ++i) uart[k].sent.push_back(src[i]);
                c.busy = false;
            }
        }
        if (tx && rx) runSpi(k, *tx, *rx);
    }
}

// ---- interrupts ----

static bool pending(unsigned irq) {
    if (!irqEnabled[irq]) return false;
    if (irq == I2C0_IRQ || irq == I2C1_IRQ) { const I2cBlock& b = i2c[irq - I2C0_IRQ]; return (b.raw() & b.mask) != 0; }
    if (irq == DMA_IRQ_0 || irq == DMA_IRQ_1) { for (auto& c : dma) if (c.status[irq - DMA_IRQ_0] && c.irqLine[irq - DMA_IRQ_0]) return true; return false; }
    if (irq == UART0_IRQ || irq == UART1_IRQ) { const UartBlock& b = uart[irq - UART0_IRQ]; return b.rxIrq && !b.rx.empty(); }
    return false;
}

void step() {
    i2c[0].step(); i2c[1].step();
    stepDma();
    // As the NVIC would: run a line's handlers while it is still asserted.
    // A line that stays asserted after its handlers ran is a storm; the
    // cap stops the test hanging on one.
    for (unsigned irq = 0; irq < 32; ++irq) {
        for (int guard = 0; guard < 4 && pending(irq); ++guard) {
            for (auto h : handlers[irq]) { ++isrCalls; h(); }
        }
    }
}

void steps(unsigned n) { for (unsigned i = 0; i < n; ++i) step(); }

} // namespace sim

// ---- the SDK functions ----

void irq_add_shared_handler(uint num, irq_handler_t handler, uint8_t order_priority) {
    (void)order_priority;
    sim::handlers[num].push_back(handler);
}
void irq_set_enabled(uint num, bool enabled) { sim::irqEnabled[num] = enabled; }

int i2c_read_timeout_us(i2c_inst_t* i, uint8_t addr, uint8_t* dst, size_t len, bool nostop, uint timeout_us) {
    (void)nostop; (void)timeout_us;
    sim::RegDevice* dev = sim::i2c[i2c_get_index(i)].devices[addr & 0x7F];
    if (dev == nullptr) return -1;
    for (size_t n = 0; n < len; ++n) dst[n] = 0;
    return static_cast<int>(len);
}

int dma_claim_unused_channel(bool required) {
    (void)required;
    int inUse = 0;
    for (auto& c : sim::dma) inUse += c.claimed;
    if (inUse >= sim::dmaChannelsAvailable) return -1;
    for (int ch = 0; ch < 12; ++ch) if (!sim::dma[ch].claimed) { sim::dma[ch].claimed = true; return ch; }
    return -1;
}
void dma_channel_unclaim(uint channel) { sim::dma[channel] = sim::DmaChannel(); }

void dma_channel_configure(uint channel, const dma_channel_config* config, volatile void* write_addr,
                           const volatile void* read_addr, uint32_t transfer_count, bool trigger) {
    sim::DmaChannel& c = sim::dma[channel];
    c.write = write_addr; c.read = read_addr; c.count = transfer_count;
    c.readInc = config->readInc; c.writeInc = config->writeInc; c.dreq = config->dreq;
    if (trigger) c.busy = true;
}
void dma_start_channel_mask(uint32_t chan_mask) {
    for (unsigned ch = 0; ch < 12; ++ch) if (chan_mask & (1u << ch)) sim::dma[ch].busy = true;
}
