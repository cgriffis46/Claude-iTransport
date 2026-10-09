#include "PicoUartTransport.h"
#include "hardware/dma.h"
#include "hardware/irq.h"

namespace {

constexpr unsigned kUarts = 2;
PicoUartTransport* s_active[kUarts] = {nullptr, nullptr};
bool s_irqInstalled[kUarts] = {false, false};

void irq0() { PicoUartTransport::handleIrq(0); }
void irq1() { PicoUartTransport::handleIrq(1); }

} // namespace

PicoUartTransport::PicoUartTransport(uart_inst_t* uart) : uart_(uart) {
    const unsigned index = uart_get_index(uart);
    s_active[index] = this;
    txChan_ = dma_claim_unused_channel(false);   // none left: write() will refuse
    if (!s_irqInstalled[index]) {
        const unsigned irq = UART0_IRQ + index;
        irq_add_shared_handler(irq, index == 0 ? irq0 : irq1, PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY);
        irq_set_enabled(irq, true);
        s_irqInstalled[index] = true;
    }
    startReceiving();
}

PicoUartTransport::~PicoUartTransport() {
    const unsigned index = uart_get_index(uart_);
    if (s_active[index] == this) {
        if (uart_is_enabled(uart_)) uart_set_irqs_enabled(uart_, false, false);
        s_active[index] = nullptr;
    }
    if (txChan_ >= 0) {
        dma_channel_abort(static_cast<unsigned>(txChan_));
        dma_channel_unclaim(static_cast<unsigned>(txChan_));
    }
}

void PicoUartTransport::setRxSink(iTransportRxSink& sink) {
    rxSink_ = &sink;
    startReceiving();
}

// The receive interrupts (data, and the timeout for a quiet line with
// bytes still waiting) are what reception is here. uart_init() clears
// them, so a transport built before it is started again from here.
bool PicoUartTransport::startReceiving() {
    if (!uart_is_enabled(uart_)) return false;
    uart_set_irqs_enabled(uart_, true, false);
    return true;
}

bool PicoUartTransport::isSending() const {
    return txChan_ >= 0 && dma_channel_is_busy(static_cast<unsigned>(txChan_));
}

bool PicoUartTransport::write(const uint8_t* data, size_t len) {
    if (data == nullptr || len == 0 || txChan_ < 0 || isSending()) return false;
    uart_hw_t* hw = uart_get_hw(uart_);

    dma_channel_config c = dma_channel_get_default_config(static_cast<unsigned>(txChan_));
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, uart_get_dreq(uart_, true));
    dma_channel_configure(static_cast<unsigned>(txChan_), &c, &hw->dr, data,
                          static_cast<uint32_t>(len), true);
    return true;
}

void PicoUartTransport::handleIrq(unsigned index) {
    PicoUartTransport* t = s_active[index];
    if (t != nullptr) t->handleRx();
}

// Interrupt context. Empties the receive FIFO into the sink. Reading
// the data register clears the receive interrupts; an overrun (bytes
// lost because the FIFO was full) is cleared separately.
void PicoUartTransport::handleRx() {
    uart_hw_t* hw = uart_get_hw(uart_);
    while (!(hw->fr & UART_UARTFR_RXFE_BITS)) {
        const uint32_t d = hw->dr;
        if (rxSink_ != nullptr) rxSink_->onByteReceived(static_cast<uint8_t>(d & 0xFF));
    }
    hw->icr = UART_UARTICR_OEIC_BITS;
}
