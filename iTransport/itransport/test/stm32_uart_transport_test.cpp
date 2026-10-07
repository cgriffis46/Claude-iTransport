// Host test for Stm32HalUartTransport — the real file, with its real
// interrupt callbacks, run against a simulated HAL UART (stub_hal/).
// Build as one line:
//
//   g++ -std=c++17 -Wall -Wextra -Istub_hal -I../inc -I../hw/stm32/inc stm32_uart_transport_test.cpp
//       stub_hal/fake_hal.cpp ../hw/stm32/src/Stm32HalUartTransport.cpp ../hw/stm32/src/Stm32UartItCallbacks.cpp
//       ../src/BusTransport.cpp ../src/OneWireUartTransport.cpp -o stm32_uart_transport_test
#include <cstdio>
#include <vector>
#include "Stm32HalUartTransport.h"

static int g_failures = 0;
static void check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failures;
}

struct Collector : iTransportRxSink {
    std::vector<uint8_t> got;
    void onByteReceived(uint8_t b) override { got.push_back(b); }
};

int main() {
    using Bytes = std::vector<uint8_t>;

    std::printf("receiving\n");
    {
        UART_HandleTypeDef huart = {}; fakeUartInit(&huart);
        Stm32HalUartTransport uart(&huart);
        check(huart.RxState == HAL_UART_STATE_BUSY_RX, "listening as soon as it is constructed");
        fakeRxByte(&huart, 0x11);
        Collector sink; uart.setRxSink(sink);
        for (uint8_t b : {0x42, 0x4D, 0x00}) fakeRxByte(&huart, b);
        check(sink.got == Bytes({0x42, 0x4D, 0x00}), "bytes go to the sink in order; one that came before a sink was attached is dropped");
        check(huart.bytesLost == 0, "the receive is armed again after every byte");
    }

    std::printf("overrun\n");
    {
        UART_HandleTypeDef huart = {}; fakeUartInit(&huart);
        Stm32HalUartTransport uart(&huart);
        Collector sink; uart.setRxSink(sink);
        fakeRxByte(&huart, 0x01);
        fakeOverrun(&huart);                 // HAL abandons the receive here
        check(huart.RxState == HAL_UART_STATE_BUSY_RX, "listening again straight after the overrun");
        for (uint8_t b : {0x02, 0x03}) fakeRxByte(&huart, b);
        check(sink.got == Bytes({0x01, 0x02, 0x03}) && huart.bytesLost == 0, "everything that arrives afterwards is received");
    }
    {
        UART_HandleTypeDef huart = {}; fakeUartInit(&huart);
        Stm32HalUartTransport uart(&huart);
        Collector sink; uart.setRxSink(sink);
        for (int i = 0; i < 5; ++i) { fakeOverrun(&huart); fakeRxByte(&huart, (uint8_t)i); }
        check(sink.got.size() == 5, "and after overrun upon overrun");
    }
    {
        UART_HandleTypeDef huart = {}; fakeUartInit(&huart);
        huart.overrunPending = 1;            // left set by something earlier
        Stm32HalUartTransport uart(&huart);
        Collector sink; uart.setRxSink(sink);
        fakeRxByte(&huart, 0x55);
        check(sink.got == Bytes({0x55}), "an overrun flag left pending does not stop the receive being armed");
    }

    std::printf("noise or framing flag\n");
    {
        UART_HandleTypeDef huart = {}; fakeUartInit(&huart);
        Stm32HalUartTransport uart(&huart);
        Collector sink; uart.setRxSink(sink);
        const int armed = huart.receivesArmed;
        fakeNoise(&huart);
        check(huart.receivesArmed == armed && huart.RxState == HAL_UART_STATE_BUSY_RX, "the receive in progress is left alone");
        fakeRxByte(&huart, 0x77);
        check(sink.got == Bytes({0x77}), "and the byte still arrives");
    }

    std::printf("UART not initialised when the transport is constructed\n");
    {
        UART_HandleTypeDef huart = {};       // as a global built before main() would find it
        Stm32HalUartTransport uart(&huart);
        Collector sink; uart.setRxSink(sink);
        check(huart.RxState == HAL_UART_STATE_RESET && !uart.startReceiving(), "nothing armed, and startReceiving() says so");
        fakeUartInit(&huart);                // MX_USARTx_UART_Init() runs later
        uart.setRxSink(sink);                // a driver calling this again once it has heard nothing
        fakeRxByte(&huart, 0x99);
        check(sink.got == Bytes({0x99}), "attaching the sink again starts the receiver");
    }

    std::printf("sending\n");
    {
        UART_HandleTypeDef huart = {}; fakeUartInit(&huart);
        Stm32HalUartTransport uart(&huart);
        static const uint8_t msg[3] = {1, 2, 3};
        check(uart.write(msg, 3) && huart.txPtr == msg && huart.txLen == 3, "write() hands the bytes to HAL");
        check(!uart.write(msg, 3), "a second write while the first is in flight is refused");
        fakeTxDone(&huart);
        check(uart.write(msg, 3), "and accepted once the first has gone");
        fakeTxDone(&huart);
        check(!uart.write(nullptr, 3) && !uart.write(msg, 0), "null and zero-length writes are refused");
    }
    {
        UART_HandleTypeDef huart = {}; fakeUartInit(&huart);
        huart.txCompletesAtOnce = 1;         // the interrupt lands before HAL_UART_Transmit_IT returns
        Stm32HalUartTransport uart(&huart);
        static const uint8_t msg[2] = {0xAA, 0x55};
        bool all = true;
        for (int i = 0; i < 100; ++i) if (!uart.write(msg, 2)) all = false;
        check(all, "transmit-complete arriving before write() returns: every one of 100 writes still goes out");
    }

    std::printf("lifetime\n");
    {
        UART_HandleTypeDef huart = {}; fakeUartInit(&huart);
        Collector sink;
        {
            Stm32HalUartTransport uart(&huart);
            uart.setRxSink(sink);
            fakeRxByte(&huart, 0x01);
        }
        check(huart.receivesAborted == 1 && huart.RxState == HAL_UART_STATE_READY, "destroying the transport cancels its receive");
        fakeRxByte(&huart, 0x02);            // must not reach the dead object
        HAL_UART_ErrorCallback(&huart);
        check(sink.got == Bytes({0x01}) && huart.RxState == HAL_UART_STATE_READY, "interrupts for that UART afterwards go nowhere");
        Stm32HalUartTransport next(&huart);
        Collector sink2; next.setRxSink(sink2);
        fakeRxByte(&huart, 0x03);
        check(sink2.got == Bytes({0x03}), "a new transport on the same UART works");
    }

    std::printf("\n%s\n", g_failures ? "FAILED" : "all passed");
    return g_failures ? 1 : 0;
}
