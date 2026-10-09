#pragma once
// A small simulation of the RP2040 peripherals the Pico transports use
// (I2C controller, SPI, UART, DMA, GPIO, interrupts, mutex), behind
// headers with the Pico SDK's names, so PicoI2CTransport.cpp,
// PicoSPITransport.cpp and PicoUartTransport.cpp build and run on a
// PC unchanged.
//
// Registers are proxies: reading or writing one runs the simulation,
// as on the chip (a write to data_cmd queues a command, a read of
// clr_tx_abrt clears the abort). Nothing happens on its own: the test
// calls sim::step(), which moves each peripheral on by one byte and
// then runs whichever interrupt handlers have something pending, as
// the NVIC would.
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

typedef unsigned int uint;

// Which chip: the RP2040 by default, the RP2350 with -DSIM_RP2350.
// They share the I2C, SPI and UART blocks; the RP2350 has 16 DMA
// channels and 4 DMA interrupt lines instead of 12 and 2, and its own
// interrupt numbers (hardware/irq.h).
#ifdef SIM_RP2350
#define NUM_DMA_CHANNELS 16u
#define NUM_DMA_IRQS     4u
#else
#define NUM_DMA_CHANNELS 12u
#define NUM_DMA_IRQS     2u
#endif
#define NUM_I2CS  2u
#define NUM_SPIS  2u
#define NUM_UARTS 2u
#define SIM_NUM_IRQS 64u

// A register: reading or writing it calls into the simulation.
struct SimReg {
    std::function<uint32_t()>     rd;
    std::function<void(uint32_t)> wr;
    operator uint32_t() const { return rd ? rd() : 0; }
    SimReg& operator=(uint32_t v) { if (wr) wr(v); return *this; }
    SimReg& operator=(const SimReg& o) { return *this = static_cast<uint32_t>(o); }   // copies the value, as on a chip
    // Wires the register to the simulation.
    void bind(std::function<uint32_t()> r, std::function<void(uint32_t)> w) { rd = std::move(r); wr = std::move(w); }
};

namespace sim {

// ---- devices on the buses ----

// A register-addressed chip, as on I2C (the first byte written after a
// start is the register pointer) or SPI (the first byte is the address,
// bit 7 set to read). Every event is logged, so a test can check the
// shape of a transaction.
struct RegDevice {
    uint8_t reg[256] = {0};
    uint8_t ptr = 0;
    bool    first = true;
    std::vector<std::string> log;     // "S" start, "R" repeated start, "P" stop, "w12", "r34"
    void start(bool restart) { first = true; log.push_back(restart ? "R" : "S"); }
    void stop() { log.push_back("P"); }
    void write(uint8_t b) {
        log.push_back("w" + std::to_string(b));
        if (first) { ptr = b; first = false; } else { reg[ptr++] = b; }
    }
    uint8_t read() { log.push_back("r"); first = false; return reg[ptr++]; }
    std::string trace() const { std::string s; for (auto& e : log) { if (!s.empty()) s += ' '; s += e; } return s; }
};

// ---- I2C controller ----
struct I2cBlock {
    bool enabled = false;
    uint32_t tar = 0, mask = 0x8FF, rx_tl = 0, tx_tl = 0;
    std::deque<uint32_t> tx;
    std::deque<uint8_t>  rx;
    bool abrt = false, stopDet = false, inTxn = false, lastWasRead = false;
    bool rxOverflow = false, txOverflow = false;
    RegDevice* devices[128] = {};
    unsigned bytePeriod = 1, ticks = 0;     // ticks per byte on the wire
    uint32_t raw() const;
    void step();
};

// ---- SPI ----
struct SpiBlock {
    std::deque<uint8_t> rx;              // the receive FIFO, outside DMA
    RegDevice* devices[32] = {};         // by chip-select GPIO
    bool       overrunCleared = false;
};

// ---- UART ----
struct UartBlock {
    bool enabled = false, rxIrq = false;
    std::deque<uint8_t> rx;              // up to 32 deep
    std::vector<uint8_t> sent;
    bool overrun = false;
};

// ---- DMA ----
struct DmaChannel {
    bool claimed = false, busy = false, irqLine[NUM_DMA_IRQS] = {}, status[NUM_DMA_IRQS] = {};
    volatile void* write = nullptr; const volatile void* read = nullptr;
    uint32_t count = 0; bool readInc = true, writeInc = true; uint dreq = 0;
};

extern I2cBlock   i2c[2];
extern SpiBlock   spi[2];
extern UartBlock  uart[2];
extern DmaChannel dma[NUM_DMA_CHANNELS];
extern bool       gpioOut[32];
extern int        gpioLevel[32];        // -1 until driven
extern int        dmaChannelsAvailable; // to test running out
extern unsigned   isrCalls;

void reset();                            // peripherals, DMA and GPIO; not interrupt handlers
void step();                             // one tick: peripherals, then interrupts
void steps(unsigned n);

} // namespace sim
