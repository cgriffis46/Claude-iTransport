#pragma once
#include <cstddef>
#include <cstdint>
#include "hardware/uart.h"
#include "iTransport.h"

// A UART byte stream on the RP2040, through the Pico SDK, without
// blocking: the RP2040's counterpart of Stm32HalUartTransport.
//
//   uart_init(uart1, 9600);                        // and the pins, as usual
//   gpio_set_function(4, GPIO_FUNC_UART);
//   gpio_set_function(5, GPIO_FUNC_UART);
//   static PicoUartTransport link(uart1);          // on its own, or
//   static PM25<PicoUartTransport> pm25(uart1);    // as a driver's transport
//
// One transport per UART: the interrupt goes to the last one made.
//
// Receiving: the UART interrupt hands each byte to the sink attached
// with setRxSink(), straight from interrupt context, as soon as the
// receive FIFO has some (or has held some for a while: the receive
// timeout interrupt catches the last bytes of a burst). Until a sink is
// attached, bytes are dropped.
//
// Sending: write() starts a DMA channel feeding the transmit FIFO and
// returns at once. As with Stm32HalUartTransport, `data` is sent from
// where it is, so it must stay valid until the send is done; write()
// refuses a new send while the last is still going.
//
// The UART interrupt is installed as a shared handler, and a DMA
// channel claimed, by the constructor. Reception starts there if the
// UART is already initialised, and otherwise on setRxSink() or
// startReceiving() once it is.
class PicoUartTransport : public iTransport {
public:
    explicit PicoUartTransport(uart_inst_t* uart);
    ~PicoUartTransport() override;

    // The interrupt holds a pointer to this object.
    PicoUartTransport(const PicoUartTransport&) = delete;
    PicoUartTransport& operator=(const PicoUartTransport&) = delete;

    bool write(const uint8_t* data, size_t len) override;

    // Attaches the sink and makes sure reception is running. Calling
    // it again is harmless.
    void setRxSink(iTransportRxSink& sink) override;

    // Starts reception if the UART is initialised. True if it is now
    // listening.
    bool startReceiving();

    // True while a send is still going.
    bool isSending() const;

    // The UART interrupt, for the UART with index `index` (0 or 1).
    // Installed by the constructor; public for a test to call.
    static void handleIrq(unsigned index);

private:
    void handleRx();

    uart_inst_t*      uart_;
    iTransportRxSink* rxSink_ = nullptr;
    int               txChan_ = -1;
};
