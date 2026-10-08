#pragma once
#include <cstddef>
#include <cstdint>
#include "iBlockTransport.h"

// A serial NOR flash chip on SPI (Winbond W25Q, Macronix MX25L, GigaDevice
// GD25Q, ISSI IS25LP, Microchip SST26, and others that follow the common
// command set), as a block device for a file system (LittleFsNor):
//
//     static Stm32HalSpiBlockTransport bus(&hspi2, FLASH_CS_GPIO_Port, FLASH_CS_Pin, spi2Mutex);
//     SpiNorFlash::Config c;
//     c.yield = [](void*) { vTaskDelay(1); };              // while the chip works
//     c.now = [](void*) { return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS); };
//     static SpiNorFlash flash(bus, c);
//     flash.begin();                                       // finds the size from the JEDEC ID
//
// Unlike the drivers that run as state machines, every call here waits
// until it is done: a file system calls its block device that way. It
// waits by calling Config::yield between looks at the bus and at the
// chip's busy bit, so other threads run meanwhile. Call it from one
// thread at a time (LittleFsNor's lock sees to that).
//
// begin() wakes the chip from deep power-down, reads the JEDEC ID, takes
// the size from its capacity byte (2^n bytes), uses 4-byte addresses
// above 16 MB, and clears the block-protection bits some chips power up
// with (SST26: a global unlock). Programs are split at 256-byte page
// boundaries. Each program or erase is preceded by a write enable that
// is checked (a WP pin held low shows here), and followed by a wait on
// the busy bit, with a timeout.
class SpiNorFlash {
public:
    static constexpr uint32_t kPageBytes = 256;
    static constexpr uint32_t kSectorBytes = 4096;

    typedef void (*YieldFn)(void* ctx);
    typedef uint32_t (*ClockFn)(void* ctx);

    struct Config {
        YieldFn  yield = nullptr;         // between polls; nullptr: spin
        void*    yieldCtx = nullptr;
        ClockFn  now = nullptr;           // ms; nullptr: timeouts count polls instead
        void*    nowCtx = nullptr;
        uint32_t eraseTimeoutMs = 2000;   // a 4 KB sector: typically 45 ms, up to 400 ms
        uint32_t programTimeoutMs = 50;   // a page: typically 0.7 ms, up to 3 ms
        uint32_t busTimeoutMs = 200;      // to get the SPI bus, and for a transfer
    };

    enum class Error : uint8_t { None, NoChip, Bus, WriteProtected, Timeout, Range };

    explicit SpiNorFlash(iBlockTransport& bus) : SpiNorFlash(bus, Config()) {}
    SpiNorFlash(iBlockTransport& bus, const Config& cfg) : bus_(bus), cfg_(cfg) {}

    bool begin();

    uint32_t size() const { return size_; }        // bytes; 0 before begin()
    uint32_t jedecId() const { return jedec_; }    // manufacturer, type, capacity
    Error    error() const { return error_; }

    bool read(uint32_t addr, void* buf, size_t len);
    bool program(uint32_t addr, const void* data, size_t len);   // only clears bits: erase first
    bool erase(uint32_t addr);                                   // the 4 KB sector holding addr

private:
    bool     transfer(bool write, const uint8_t* header, uint8_t headerLen, uint8_t* data, size_t len);
    bool     command(uint8_t op);
    bool     readStatus(uint8_t& sr);
    bool     writeEnable();
    bool     waitReady(uint32_t timeoutMs);
    uint8_t  header(uint8_t op3, uint8_t op4, uint32_t addr, uint8_t* h) const;
    uint32_t nowMs() const;
    void     yield() const { if (cfg_.yield) cfg_.yield(cfg_.yieldCtx); }
    bool     fail(Error e) { error_ = e; return false; }

    iBlockTransport& bus_;
    Config           cfg_;
    uint32_t         size_ = 0;
    uint32_t         jedec_ = 0;
    bool             fourByte_ = false;
    Error            error_ = Error::None;
    uint32_t         polls_ = 0;    // the clock when Config::now is missing
};
