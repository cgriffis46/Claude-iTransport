// Host test for the RP2040 transports: PicoI2CTransport,
// PicoSPITransport and PicoUartTransport, unchanged, over a simulation
// of the RP2040's I2C controller, SPI, UART and DMA (sim/). Build as
// one line, from this folder:
//
//   g++ -std=c++17 -Wall -Wextra -Isim -I../../inc -I../../hw/rp2040/inc pico_transport_test.cpp
//       sim/pico_sim.cpp ../../src/BusTransport.cpp ../../src/I2CTransport.cpp ../../src/SPITransport.cpp
//       ../../hw/rp2040/src/PicoI2CTransport.cpp ../../hw/rp2040/src/PicoSPITransport.cpp
//       ../../hw/rp2040/src/PicoUartTransport.cpp -o pico_transport_test
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "sim/pico_sim.h"
#include "hardware/i2c.h"
#include "hardware/irq.h"
#include "hardware/spi.h"
#include "hardware/uart.h"
#include "PicoI2CTransport.h"
#include "PicoSPITransport.h"
#include "PicoUartTransport.h"

static int g_failures = 0;
static void check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failures;
}

// Runs the simulation until the transport's transfer lands, as a
// driver's wait state would by calling isBusy() each pass.
template <typename T>
static bool land(T& t, unsigned maxTicks = 1000) {
    for (unsigned i = 0; i < maxTicks; ++i) {
        if (!t.isBusy()) return true;
        sim::step();
    }
    return !t.isBusy();
}

static std::string bytes(const uint8_t* p, size_t n) {
    std::string s;
    for (size_t i = 0; i < n; ++i) { char b[4]; std::snprintf(b, sizeof b, "%02X", p[i]); s += b; }
    return s;
}

// ---------------------------------------------------------------- I2C

static void i2cTests() {
    std::printf("PicoI2CTransport: register writes and reads\n");
    {
        sim::reset();
        sim::RegDevice chip;
        sim::i2c[0].devices[0x44] = &chip;
        PicoI2CTransport t(i2c0, 0x44);
        check(sim::irqEnabled[I2C0_IRQ] && sim::handlers[I2C0_IRQ].size() == 1, "the I2C0 interrupt is installed and enabled");

        const uint8_t w[3] = {0x11, 0x22, 0x33};
        check(t.writeRegs(0x20, w, 3), "writeRegs() starts");
        check(land(t) && !t.lastOpFailed(), "lands without error");
        check(chip.reg[0x20] == 0x11 && chip.reg[0x21] == 0x22 && chip.reg[0x22] == 0x33, "the three bytes are in registers 0x20 to 0x22");
        check(chip.trace() == "S w32 w17 w34 w51 P", "one transaction: start, register, data, stop");

        chip.log.clear();
        chip.reg[0x40] = 0xA1; chip.reg[0x41] = 0xB2; chip.reg[0x42] = 0xC3; chip.reg[0x43] = 0xD4;
        uint8_t r[4] = {0};
        check(t.readRegs(0x40, r, 4) && land(t) && !t.lastOpFailed(), "readRegs() lands without error");
        check(bytes(r, 4) == "A1B2C3D4", "reads the four registers");
        check(chip.trace() == "S w64 R r r r r P", "register, repeated start, four reads, stop");
        check(sim::i2c[0].mask == 0, "interrupts masked again once idle");
    }

    std::printf("PicoI2CTransport: command-style chips (no register address)\n");
    {
        sim::reset();
        sim::RegDevice chip;
        sim::i2c[0].devices[0x40] = &chip;
        PicoI2CTransport t(i2c0, 0x40);
        const uint8_t cmd[2] = {0x24, 0x00};
        check(t.writeBytes(cmd, 2) && land(t) && !t.lastOpFailed(), "writeBytes() lands");
        check(chip.trace() == "S w36 w0 P", "just the two bytes, between start and stop");
        chip.log.clear(); chip.ptr = 0x10; chip.reg[0x10] = 0x66; chip.reg[0x11] = 0x77;
        uint8_t r[2] = {0};
        check(t.readBytes(r, 2) && land(t) && bytes(r, 2) == "6677", "readBytes() reads the bytes as they come");
        check(chip.trace() == "S r r P", "no address and no repeated start");
    }

    std::printf("PicoI2CTransport: the longest transfers, through the 16 entry FIFOs\n");
    {
        sim::reset();
        sim::RegDevice chip;
        sim::i2c[1].devices[0x30] = &chip;
        sim::i2c[1].bytePeriod = 4;     // a slow bus, so the interrupt has to keep up
        PicoI2CTransport t(i2c1, 0x30);
        uint8_t w[32];
        for (int i = 0; i < 32; ++i) w[i] = static_cast<uint8_t>(0x80 + i);
        check(t.writeRegs(0x00, w, 32) && land(t) && !t.lastOpFailed(), "a 32 byte write (33 commands) lands");
        bool same = true; for (int i = 0; i < 32; ++i) same = same && chip.reg[i] == w[i];
        check(same && !sim::i2c[1].txOverflow, "all 32 bytes written, and the transmit FIFO never overfilled");

        for (int i = 0; i < 32; ++i) chip.reg[0x60 + i] = static_cast<uint8_t>(i * 7);
        uint8_t r[32] = {0};
        sim::isrCalls = 0;
        check(t.readRegs(0x60, r, 32) && land(t) && !t.lastOpFailed(), "a 32 byte read lands");
        same = true; for (int i = 0; i < 32; ++i) same = same && r[i] == static_cast<uint8_t>(i * 7);
        check(same && !sim::i2c[1].rxOverflow, "all 32 bytes read, and the receive FIFO never overflowed");
        std::printf("        %u interrupts for 33 bytes on the wire\n", sim::isrCalls);
        check(sim::isrCalls <= 40, "about one interrupt per byte: none while waiting for reads to come back");
    }

    std::printf("PicoI2CTransport: no device at the address (NACK)\n");
    {
        sim::reset();
        sim::RegDevice chip;
        sim::i2c[0].devices[0x44] = &chip;
        PicoI2CTransport absent(i2c0, 0x45);
        PicoI2CTransport present(i2c0, 0x44);
        check(sim::handlers[I2C0_IRQ].size() == 1, "the interrupt is installed once per I2C block");
        uint8_t r[2];
        check(absent.readRegs(0x00, r, 2) && land(absent), "the transfer still lands");
        check(absent.lastOpFailed(), "and reports failure");
        check(!absent.checkDevice() && present.checkDevice(), "checkDevice(): false for 0x45, true for 0x44");
        check(present.writeReg(0x05, 0x99) && land(present) && !present.lastOpFailed() && chip.reg[0x05] == 0x99,
              "the next transfer on the bus, to the device that is there, works");
    }

    std::printf("PicoI2CTransport: sharing a bus\n");
    {
        sim::reset();
        sim::RegDevice a, b;
        sim::i2c[0].devices[0x10] = &a;
        sim::i2c[0].devices[0x11] = &b;
        mutex_t m; mutex_init(&m);
        PicoI2CTransport ta(i2c0, 0x10, &m);
        PicoI2CTransport tb(i2c0, 0x11, &m);
        check(ta.writeReg(0x01, 0xAA), "first device starts");
        check(!tb.writeReg(0x01, 0xBB), "second device is refused while the first is in flight");
        check(land(ta) && !m.held, "the first lands and the mutex is given back");
        check(tb.writeReg(0x01, 0xBB) && land(tb) && a.reg[1] == 0xAA && b.reg[1] == 0xBB, "then the second goes, each to its own device");
        m.held = true;   // the other core has the bus
        check(!ta.writeReg(0x02, 0x01), "refused, without waiting, while the other core holds the mutex");
        m.held = false;
        check(ta.writeReg(0x02, 0x01) && land(ta), "goes once it is free");
    }

    std::printf("PicoI2CTransport: interrupt with nothing in flight\n");
    {
        sim::reset();
        PicoI2CTransport t(i2c0, 0x44);
        sim::i2c[0].enabled = true;           // as after i2c_init(): reset mask, TX_EMPTY asserted
        sim::i2c[0].mask = 0x8FF;
        sim::step();
        check(sim::i2c[0].mask == 0 && sim::isrCalls == 1, "masks the block's interrupts at once, instead of storming");
    }
}

// ---------------------------------------------------------------- SPI

static void spiTests() {
    std::printf("PicoSPITransport: register writes and reads over DMA\n");
    {
        sim::reset();
        sim::RegDevice chip;
        sim::spi[0].devices[17] = &chip;
        PicoSPITransport t(spi0, 17);
        check(sim::gpioOut[17] && sim::gpioLevel[17] == 1, "chip-select is an output, high");
        check(sim::irqEnabled[DMA_IRQ_0], "DMA_IRQ_0 is installed and enabled");

        const uint8_t w[2] = {0x5A, 0xA5};
        check(t.writeRegs(0x10, w, 2), "writeRegs() starts");
        check(sim::gpioLevel[17] == 0, "chip-select low during the transfer");
        check(land(t) && !t.lastOpFailed(), "lands");
        check(sim::gpioLevel[17] == 1, "chip-select high again once it has landed");
        check(chip.reg[0x10] == 0x5A && chip.reg[0x11] == 0xA5, "both bytes written");
        check(chip.trace() == "S a16 w90 w165 P", "address with bit 7 clear (write), then the data");

        chip.log.clear(); chip.reg[0x20] = 0x01; chip.reg[0x21] = 0x02; chip.reg[0x22] = 0x03;
        uint8_t r[3] = {0};
        check(t.readRegs(0x20, r, 3) && land(t) && bytes(r, 3) == "010203", "readRegs() returns the three registers");
        check(chip.trace() == "S a160 r r r P", "address with bit 7 set (read)");
    }

    std::printf("PicoSPITransport: leftovers in the receive FIFO\n");
    {
        sim::reset();
        sim::RegDevice chip;
        sim::spi[0].devices[5] = &chip;
        PicoSPITransport t(spi0, 5);
        chip.reg[0x00] = 0x42;
        sim::spi[0].rx = {0xEE, 0xEE};       // left over from some earlier use of the bus
        uint8_t r = 0;
        check(t.readRegs(0x00, &r, 1) && land(t) && r == 0x42, "drained first, so the byte read is the register's");
        check(sim::spi[0].overrunCleared, "and a stale overrun is cleared");
    }

    std::printf("PicoSPITransport: two devices on one bus\n");
    {
        sim::reset();
        sim::RegDevice a, b;
        sim::spi[1].devices[9] = &a;
        sim::spi[1].devices[10] = &b;
        PicoSPITransport ta(spi1, 9);
        PicoSPITransport tb(spi1, 10, nullptr, 1);   // on DMA_IRQ_1
        a.reg[0] = 0x11; b.reg[0] = 0x22;
        uint8_t ra = 0, rb = 0;
        check(ta.readRegs(0, &ra, 1) && land(ta) && tb.readRegs(0, &rb, 1) && land(tb), "both land");
        check(ra == 0x11 && rb == 0x22, "each reads its own device, by its own chip-select");
        check(sim::irqEnabled[DMA_IRQ_1], "a transport can use DMA_IRQ_1 instead");
    }

    std::printf("PicoSPITransport: DMA channels\n");
    {
        sim::reset();
        {
            PicoSPITransport t(spi0, 3);
            int claimed = 0; for (auto& c : sim::dma) claimed += c.claimed;
            check(claimed == 2, "two channels claimed");
        }
        int claimed = 0; for (auto& c : sim::dma) claimed += c.claimed;
        check(claimed == 0, "and given back by the destructor");
        sim::dmaChannelsAvailable = 1;
        PicoSPITransport starved(spi0, 3);
        const uint8_t w = 1;
        check(!starved.writeRegs(0, &w, 1), "with no channels to be had, transfers are refused rather than crash");
        claimed = 0; for (auto& c : sim::dma) claimed += c.claimed;
        check(claimed == 0, "and the one channel it did get is given back");
    }
}

// --------------------------------------------------------------- UART

struct Sink : iTransportRxSink {
    std::vector<uint8_t> got;
    void onByteReceived(uint8_t b) override { got.push_back(b); }
};

static void uartTests() {
    std::printf("PicoUartTransport: receiving\n");
    {
        sim::reset();
        PicoUartTransport t(uart1);     // before uart_init(), as for an object made before main()
        check(sim::irqEnabled[UART1_IRQ] && !sim::uart[1].rxIrq, "interrupt installed; reception waits for the UART");
        sim::uart[1].enabled = true;    // uart_init()
        Sink sink;
        t.setRxSink(sink);
        check(sim::uart[1].rxIrq, "setRxSink() starts reception");
        const char* msg = "BM\x00\x1c";
        for (int i = 0; i < 4; ++i) sim::uart[1].rx.push_back(static_cast<uint8_t>(msg[i]));
        sim::step();
        check(sink.got.size() == 4 && sink.got[0] == 'B' && sink.got[3] == 0x1C, "the bytes reach the sink, in order");
        for (int i = 0; i < 30; ++i) sim::uart[1].rx.push_back(static_cast<uint8_t>(i));
        sim::step();
        check(sink.got.size() == 34 && sim::uart[1].rx.empty(), "a full FIFO's worth in one interrupt");
    }

    std::printf("PicoUartTransport: no sink yet\n");
    {
        sim::reset();
        sim::uart[0].enabled = true;
        PicoUartTransport t(uart0);
        check(sim::uart[0].rxIrq, "receiving from the start when the UART is already up");
        sim::uart[0].rx.push_back(0x55);
        sim::step();
        check(sim::uart[0].rx.empty(), "a byte with no sink is dropped, and the FIFO doesn't fill");
        Sink sink; t.setRxSink(sink);
        sim::uart[0].rx.push_back(0x66);
        sim::step();
        check(sink.got.size() == 1 && sink.got[0] == 0x66, "a sink attached later gets what comes after");
    }

    std::printf("PicoUartTransport: sending\n");
    {
        sim::reset();
        sim::uart[0].enabled = true;
        PicoUartTransport t(uart0);
        static const uint8_t a[3] = {1, 2, 3};
        static const uint8_t b[2] = {9, 8};
        check(t.write(a, 3) && t.isSending(), "write() starts a DMA send");
        check(!t.write(b, 2), "a second write() is refused while it is going");
        sim::step();
        check(!t.isSending() && t.write(b, 2), "accepted once the first is done");
        sim::step();
        check(sim::uart[0].sent == std::vector<uint8_t>({1, 2, 3, 9, 8}), "both sends went out, in order");
        check(!t.write(nullptr, 1) && !t.write(a, 0), "nothing to send is refused");
    }
}

int main() {
    i2cTests();
    spiTests();
    uartTests();
    std::printf("\n%s\n", g_failures ? "FAILED" : "all passed");
    return g_failures ? 1 : 0;
}
