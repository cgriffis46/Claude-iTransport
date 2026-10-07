// Host test for FreeRtosTransport<TBus>, the CMSIS-RTOS2 layer shared
// by I2C, SPI and 1-Wire, against a small simulation of the RTOS calls it
// makes (stub/cmsis_os2.h). Build as one line:
//
//   g++ -std=c++17 -Wall -Wextra -I../inc -I../hw/freertos/inc -Istub freertos_transport_test.cpp
//       ../src/BusTransport.cpp ../src/I2CTransport.cpp ../src/SPITransport.cpp
//       ../src/OneWireUartTransport.cpp -o freertos_transport_test
#include <cstdio>
#include <cstring>
#include "I2CTransport.h"
#include "SPITransport.h"
#include "OneWireUartTransport.h"
#include "FreeRtosTransport.h"

static int g_failures = 0;
static void check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failures;
}

static int g_i2c1, g_i2c2, g_spi1, g_uart1;             // stand in for &hi2c1, &hi2c2, &hspi1, &huart1
static int g_mtxI2c1, g_mtxI2c2, g_mtxSpi1, g_mtxUart1;  // stand in for four osMutexNew() results
static int g_taskA, g_taskB;                 // two threads
static const uint32_t kWake = 0x40000000u;   // the flag FreeRtosTransport uses

class RtosI2C : public FreeRtosTransport<I2CTransport> {
public:
    RtosI2C(void* bus, void* mutex) : FreeRtosTransport<I2CTransport>(bus, 0x38, mutex), bus_(bus) {}
    void irq(bool failed = false) { BusTransport::onTransferComplete(bus_, failed); } // the I2C interrupt
protected:
    bool halMemWrite(uint8_t, uint8_t*, uint16_t) override { return true; }
    bool halMemRead(uint8_t, uint8_t*, uint16_t) override { return true; }
    bool halMasterTransmit(uint8_t*, uint16_t) override { return true; }
    bool halMasterReceive(uint8_t*, uint16_t) override { return true; }
    bool halIsDeviceReady(uint32_t, uint32_t) override { return true; }
private:
    void* bus_;
};

class RtosSPI : public FreeRtosTransport<SPITransport> {
public:
    RtosSPI(void* bus, void* mutex) : FreeRtosTransport<SPITransport>(bus, nullptr, 0, mutex), bus_(bus) {}
    void irq(bool failed = false) { BusTransport::onTransferComplete(bus_, failed); } // the SPI interrupt
    bool csLow = false; int csFalls = 0;
    bool busHeldWhenCsFell = false;
protected:
    bool halTransmit(uint8_t*, uint16_t) override { return true; }
    bool halTransmitReceive(uint8_t*, uint8_t* rx, uint16_t n) override { std::memset(rx, 0xC3, n); return true; }
    void halCsLow() override { csLow = true; ++csFalls; busHeldWhenCsFell = (g_rtos.owner[&g_mtxSpi1] == g_rtos.current); }
    void halCsHigh() override { csLow = false; }
private:
    void* bus_;
};

class RtosOneWire : public FreeRtosTransport<OneWireUartTransport> {
public:
    RtosOneWire(void* uart, void* mutex) : FreeRtosTransport<OneWireUartTransport>(uart, mutex), uart_(uart) {}
    void txIrq()  { OneWireUartTransport::onUartTxComplete(uart_); }  // the UART's interrupts
    void rxIrq()  { OneWireUartTransport::onUartRxComplete(uart_); }
    void errIrq() { OneWireUartTransport::onUartError(uart_); }
    void irq(bool failed = false) { if (failed) errIrq(); else { rxIrq(); txIrq(); } }
    using OneWireUartTransport::writeBytes;
protected:
    bool halSetBaud(uint32_t) override { return true; }
    bool halReceive(uint8_t* rx, uint16_t n) override { rx_ = rx; n_ = n; return true; }
    bool halTransmit(uint8_t* tx, uint16_t n) override { std::memcpy(rx_, tx, n); return true; } // plain echo
    void halAbort() override {}
private:
    void* uart_; uint8_t* rx_ = nullptr; uint16_t n_ = 0;
};

template <typename TBus>
void waitsAndWakes(const char* name, TBus& bus, void* mutex) {
    std::printf("%s\n", name);
    uint8_t cmd[2] = {0x01, 0x02};
    g_rtos.current = &g_taskA;
    g_rtos.blocks = 0;

    check(bus.writeBytes(cmd, 2), "transfer issued");
    check(g_rtos.owner[mutex] == &g_taskA, "bus mutex held by the issuing thread");

    check(bus.isBusy() && g_rtos.blocks == 1 && g_rtos.lastTimeout == 50, "isBusy() blocks the thread (bounded) while nothing has happened");

    g_rtos.whileBlocked = [&] { bus.irq(); };
    check(!bus.isBusy() && g_rtos.blocks == 2, "the interrupt wakes it and the same isBusy() call returns done");
    check(!bus.lastOpFailed() && g_rtos.owner[mutex] == nullptr, "no error, mutex released");
    check((g_rtos.flags[&g_taskA] & kWake) == 0, "wake flag consumed");

    bus.writeBytes(cmd, 2);
    bus.irq();                                   // lands before anyone asks
    g_rtos.blocks = 0;
    check(!bus.isBusy() && g_rtos.blocks == 0, "a transfer that already landed is reported without blocking");
    g_rtos.flags[&g_taskA] = 0;

    bus.writeBytes(cmd, 2);
    g_rtos.whileBlocked = [&] { bus.irq(/*failed=*/true); };
    check(!bus.isBusy() && bus.lastOpFailed() && g_rtos.owner[mutex] == nullptr, "an error interrupt wakes it too, and is reported");
    g_rtos.flags[&g_taskA] = 0;
}

int main() {
    uint8_t cmd[2] = {0x01, 0x02};

    { RtosI2C bus(&g_i2c1, &g_mtxI2c1); waitsAndWakes("FreeRtosTransport<I2CTransport>", bus, &g_mtxI2c1); }
    { RtosSPI bus(&g_spi1, &g_mtxSpi1); waitsAndWakes("FreeRtosTransport<SPITransport>", bus, &g_mtxSpi1); }
    { RtosOneWire bus(&g_uart1, &g_mtxUart1); waitsAndWakes("FreeRtosTransport<OneWireUartTransport>", bus, &g_mtxUart1); }

    std::printf("1-Wire: one wake-up per operation, when both UART interrupts are in\n");
    {
        RtosOneWire bus(&g_uart1, &g_mtxUart1);
        g_rtos.current = &g_taskA;
        g_rtos.sets = 0;
        bus.reset();
        bus.txIrq();
        check(g_rtos.sets == 0 && bus.isBusy(), "transmit-complete alone wakes nobody");
        bus.rxIrq();
        check(g_rtos.sets == 1, "receive-complete, the second of the two, wakes the thread once");
        check(!bus.isBusy() && !bus.lastOpFailed() && g_rtos.owner[&g_mtxUart1] == nullptr, "operation done, mutex released");
        g_rtos.flags[&g_taskA] = 0;
    }

    std::printf("SPI: order of mutex and chip-select\n");
    {
        RtosSPI bus(&g_spi1, &g_mtxSpi1);
        g_rtos.current = &g_taskA;
        uint8_t buf[3] = {0};
        bus.readRegs(0x77, buf, 3);
        check(bus.busHeldWhenCsFell, "mutex taken before chip-select goes low");
        g_rtos.whileBlocked = [&] { bus.irq(); };
        check(!bus.isBusy() && !bus.csLow && buf[0] == 0xC3, "woken by the interrupt: chip-select raised, data copied out");
        check(g_rtos.owner[&g_mtxSpi1] == nullptr, "mutex released after chip-select is raised");
        g_rtos.flags[&g_taskA] = 0;
    }

    std::printf("a wake-up that isn't for this transfer\n");
    {
        RtosI2C bus(&g_i2c1, &g_mtxI2c1);
        g_rtos.current = &g_taskA;
        bus.writeBytes(cmd, 2);
        g_rtos.flags[&g_taskA] |= kWake;         // left over, or meant for something else on this thread
        check(bus.isBusy(), "is not mistaken for the transfer finishing");
        g_rtos.whileBlocked = [&] { bus.irq(); };
        check(!bus.isBusy() && !bus.lastOpFailed(), "the real interrupt still completes it");
        g_rtos.flags[&g_taskA] = 0;
    }

    std::printf("one thread driving two sensors on two buses\n");
    {
        RtosI2C a(&g_i2c1, &g_mtxI2c1), b(&g_i2c2, &g_mtxI2c2);
        g_rtos.current = &g_taskA;
        a.writeBytes(cmd, 2);
        b.writeBytes(cmd, 2);
        b.irq();                                  // B finishes first
        check(a.isBusy(), "A is still busy: B's wake-up is not taken as A's completion");
        check(!b.isBusy(), "B is done");
        a.irq();
        check(!a.isBusy() && !a.lastOpFailed(), "A is done when its own interrupt arrives");
        g_rtos.flags[&g_taskA] = 0;
    }

    std::printf("two threads sharing one bus\n");
    {
        RtosSPI a(&g_spi1, &g_mtxSpi1), b(&g_spi1, &g_mtxSpi1);
        g_rtos.current = &g_taskA;
        a.writeBytes(cmd, 2);
        g_rtos.current = &g_taskB;
        check(!b.writeBytes(cmd, 2) && b.csFalls == 0, "second thread is refused while the first holds the bus; its chip-select stays high");
        a.irq();
        check((g_rtos.flags[&g_taskA] & kWake) != 0 && (g_rtos.flags[&g_taskB] & kWake) == 0, "the interrupt wakes the thread that issued the transfer, not the other");
        g_rtos.current = &g_taskA;
        check(!a.isBusy(), "first thread sees it land");
        g_rtos.current = &g_taskB;
        check(b.writeBytes(cmd, 2), "second thread can now take the bus");
        b.irq(); b.isBusy();
        g_rtos.flags.clear();
    }

    std::printf("no mutex given\n");
    {
        RtosI2C bus(&g_i2c1, nullptr);
        g_rtos.current = &g_taskA;
        check(!bus.writeBytes(cmd, 2) && !bus.isBusy(), "every transfer is refused, visibly, rather than run unprotected");
    }

    std::printf("\n%s\n", g_failures ? "FAILED" : "all passed");
    return g_failures ? 1 : 0;
}
