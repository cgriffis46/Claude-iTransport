// A serial NOR flash chip for the host tests, behind an iBlockTransport
// (FakeNorBus), at the level of its commands: JEDEC ID, status (busy and
// WEL), write enable, write status (block protection), page program
// (ANDs into the cells, wrapping within the page), 4 KB sector erase, read,
// deep power-down and wake, 3- and 4-byte addressing, SST26's global
// unlock. Commands sent while it is busy, or programs and erases without
// write enable, are ignored as on a real chip, and counted (`misuse`), so
// a test can insist the driver never does that. Power can be cut after a
// given number of programs and erases: from then on it does nothing, and
// powerOn() brings it back with its cells as they were.
#pragma once
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>
#include "iBlockTransport.h"

class SimSpiNor {
public:
    SimSpiNor(uint32_t jedec, uint32_t size) : jedec(jedec), mem(size, 0xFF) {}

    uint32_t jedec;
    std::vector<uint8_t> mem;
    uint8_t  sr = 0;            // status register: BP bits etc. (busy and WEL kept apart)
    bool     wel = false;
    bool     asleep = false;
    bool     srLocked = false;  // status register can't be written (SRP with WP low)
    bool     wpAll = false;     // write enable never takes (WP held low on a chip that ignores WREN)
    bool     sstLocked = false; // SST26: every block protected until a global unlock
    int      busyPollsPerProgram = 2, busyPollsPerErase = 6;
    int      busyForever = 0;   // > 0: stuck busy
    int      busy = 0;          // status reads left with BUSY set

    // ---- what happened ----
    int      misuse = 0;
    int      programs = 0, erases = 0;
    uint8_t  lastOp = 0;
    long     cutAfter = -1;     // programs + erases before the power fails; -1: never
    bool     dead = false;

    void powerOn() {
        std::lock_guard<std::mutex> g(m_);
        dead = false;
        cutAfter = -1;
        wel = false;
        busy = 0;
        asleep = false;
    }

    // One transfer: header then data, as the bus carries it.
    void transfer(bool write, const uint8_t* h, uint8_t hl, uint8_t* data, size_t len) {
        std::lock_guard<std::mutex> g(m_);
        if (dead) { if (!write && data) std::memset(data, 0xFF, len); return; }
        const uint8_t op = hl ? h[0] : (len ? data[0] : 0);
        lastOp = op;
        if (asleep && op != 0xAB) { if (!write && data) std::memset(data, 0xFF, len); return; }
        if ((busy > 0 || busyForever) && op != 0x05) { ++misuse; if (!write && data) std::memset(data, 0xFF, len); return; }
        const bool four = mem.size() > (1u << 24);
        auto addr = [&](bool wide) -> uint32_t {
            if (wide) return (uint32_t(h[1]) << 24) | (uint32_t(h[2]) << 16) | (uint32_t(h[3]) << 8) | h[4];
            return (uint32_t(h[1]) << 16) | (uint32_t(h[2]) << 8) | h[3];
        };
        switch (op) {
        case 0xAB: asleep = false; return;
        case 0xB9: asleep = true; return;
        case 0x9F:
            for (size_t i = 0; i < len; ++i) data[i] = i < 3 ? uint8_t(jedec >> (16 - 8 * i)) : 0xFF;
            return;
        case 0x05:
            if (len) data[0] = uint8_t(sr | (wel ? 2 : 0) | ((busy > 0 || busyForever) ? 1 : 0));
            if (busy > 0) --busy;
            return;
        case 0x06: if (!wpAll) wel = true; return;
        case 0x04: wel = false; return;
        case 0x01:
            if (!wel) { ++misuse; return; }
            wel = false;
            if (!srLocked) sr = (len ? data[0] : 0) & 0xFC;
            busy = 1;
            return;
        case 0x98:
            if (!wel) { ++misuse; return; }
            wel = false;
            sstLocked = false;
            return;
        case 0x03: case 0x13: {
            if ((op == 0x13) != four) { ++misuse; return; }
            uint32_t a = addr(four);
            for (size_t i = 0; i < len; ++i) data[i] = mem[(a + i) % mem.size()];
            return;
        }
        case 0x02: case 0x12: {
            if ((op == 0x12) != four || !wel) { ++misuse; wel = false; return; }
            wel = false;
            if (sr & 0x7C || sstLocked) return;   // protected: nothing happens
            if (cut()) return;
            ++programs;
            const uint32_t a = addr(four);
            const uint32_t page = a & ~255u;
            for (size_t i = 0; i < len; ++i) mem[page + ((a + i) & 255u)] &= data[i];
            busy = busyPollsPerProgram;
            return;
        }
        case 0x20: case 0x21: {
            if ((op == 0x21) != four || !wel) { ++misuse; wel = false; return; }
            wel = false;
            if (sr & 0x7C || sstLocked) return;
            if (cut()) return;
            ++erases;
            const uint32_t a = addr(four) & ~4095u;
            std::memset(&mem[a], 0xFF, 4096);
            busy = busyPollsPerErase;
            return;
        }
        default:
            ++misuse;
            return;
        }
    }

private:
    // The power goes now? Then this operation never happens (a program
    // cut part way would leave some cells cleared: LittleFS copes with
    // that too, but the simple case is enough here).
    bool cut() {
        if (cutAfter < 0) return false;
        if (cutAfter == 0) { dead = true; return true; }
        --cutAfter;
        return false;
    }
    std::mutex m_;
};

// The chip's SPI bus: transfers land at once, or after a few polls of
// isBusy(), or are refused a few times (the bus in use elsewhere).
class FakeNorBus : public iBlockTransport {
public:
    explicit FakeNorBus(SimSpiNor& chip) : chip_(chip) {}
    int busyPolls = 0;       // each transfer is busy for this many isBusy() calls
    int refuse = 0;          // refuse this many starts
    bool refuseForever = false;
    bool failNext = false;
    int stuckMiso = -1;      // every byte read is this (0x00 or 0xFF: no chip)
    int transfers = 0;

    bool beginWrite(const uint8_t* h, uint8_t hl, const uint8_t* d, size_t len) override {
        if (!start()) return false;
        chip_.transfer(true, h, hl, const_cast<uint8_t*>(d), len);
        return true;
    }
    bool beginRead(const uint8_t* h, uint8_t hl, uint8_t* d, size_t len) override {
        if (!start()) return false;
        chip_.transfer(false, h, hl, d, len);
        if (stuckMiso >= 0) std::memset(d, stuckMiso, len);
        return true;
    }
    bool isBusy() const override { if (left_ > 0) { --left_; return true; } return false; }
    bool lastOpFailed() const override { return failed_; }

private:
    bool start() {
        if (refuseForever) return false;
        if (refuse > 0) { --refuse; return false; }
        ++transfers;
        left_ = busyPolls;
        failed_ = failNext;
        failNext = false;
        return true;
    }
    SimSpiNor& chip_;
    mutable int left_ = 0;
    bool failed_ = false;
};
