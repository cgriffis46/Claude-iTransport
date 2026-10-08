#include "SpiNorFlash.h"

namespace {
// The common command set.
constexpr uint8_t kWriteEnable = 0x06;
constexpr uint8_t kReadStatus = 0x05;
constexpr uint8_t kWriteStatus = 0x01;
constexpr uint8_t kRead = 0x03, kRead4 = 0x13;
constexpr uint8_t kProgram = 0x02, kProgram4 = 0x12;
constexpr uint8_t kErase4K = 0x20, kErase4K4 = 0x21;
constexpr uint8_t kJedecId = 0x9F;
constexpr uint8_t kWake = 0xAB;            // release from deep power-down
constexpr uint8_t kGlobalUnlock = 0x98;    // SST26
constexpr uint8_t kStatusBusy = 0x01, kStatusWel = 0x02;
constexpr uint8_t kStatusProtect = 0x7C;   // BP0..BP3 (or BP0..BP2, TB, SEC) and friends
constexpr uint8_t kSst = 0xBF;
}  // namespace

uint32_t SpiNorFlash::nowMs() const {
    return cfg_.now ? cfg_.now(cfg_.nowCtx) : polls_;
}

// Starts a transfer (waiting for the bus if another user has it), then
// waits for it to finish.
bool SpiNorFlash::transfer(bool write, const uint8_t* header, uint8_t headerLen, uint8_t* data, size_t len) {
    const uint32_t start = nowMs();
    for (;;) {
        const bool issued = write ? bus_.beginWrite(header, headerLen, data, len)
                                  : bus_.beginRead(header, headerLen, data, len);
        if (issued) break;
        ++polls_;
        if (nowMs() - start >= cfg_.busTimeoutMs) return fail(Error::Bus);
        yield();
    }
    while (bus_.isBusy()) {
        ++polls_;
        if (nowMs() - start >= cfg_.busTimeoutMs) return fail(Error::Bus);
        yield();
    }
    return bus_.lastOpFailed() ? fail(Error::Bus) : true;
}

bool SpiNorFlash::command(uint8_t op) {
    return transfer(true, &op, 1, nullptr, 0);
}

bool SpiNorFlash::readStatus(uint8_t& sr) {
    const uint8_t op = kReadStatus;
    return transfer(false, &op, 1, &sr, 1);
}

bool SpiNorFlash::waitReady(uint32_t timeoutMs) {
    const uint32_t start = nowMs();
    for (;;) {
        uint8_t sr = 0;
        if (!readStatus(sr)) return false;
        if (!(sr & kStatusBusy)) return true;
        ++polls_;
        if (nowMs() - start >= timeoutMs) return fail(Error::Timeout);
        yield();
    }
}

// Write enable, checked: a chip that won't set WEL is write-protected
// (its WP pin, or its status register locked).
bool SpiNorFlash::writeEnable() {
    // A busy chip ignores the command: let whatever it is doing finish.
    uint8_t sr = 0;
    if (!waitReady(cfg_.eraseTimeoutMs) || !command(kWriteEnable) || !readStatus(sr)) return false;
    return (sr & kStatusWel) ? true : fail(Error::WriteProtected);
}

uint8_t SpiNorFlash::header(uint8_t op3, uint8_t op4, uint32_t addr, uint8_t* h) const {
    if (fourByte_) {
        h[0] = op4;
        h[1] = static_cast<uint8_t>(addr >> 24);
        h[2] = static_cast<uint8_t>(addr >> 16);
        h[3] = static_cast<uint8_t>(addr >> 8);
        h[4] = static_cast<uint8_t>(addr);
        return 5;
    }
    h[0] = op3;
    h[1] = static_cast<uint8_t>(addr >> 16);
    h[2] = static_cast<uint8_t>(addr >> 8);
    h[3] = static_cast<uint8_t>(addr);
    return 4;
}

bool SpiNorFlash::begin() {
    error_ = Error::None;
    size_ = 0;
    // Awake (a chip that wasn't asleep ignores this), then who it is.
    if (!command(kWake)) return false;
    for (int i = 0; i < 20; ++i) yield();   // tRES1: a few microseconds
    uint8_t id[3] = {0, 0, 0};
    const uint8_t op = kJedecId;
    if (!transfer(false, &op, 1, id, 3)) return false;
    jedec_ = (static_cast<uint32_t>(id[0]) << 16) | (static_cast<uint32_t>(id[1]) << 8) | id[2];
    // No chip (MISO floating high or pulled low).
    if (id[0] == 0x00 || id[0] == 0xFF) return fail(Error::NoChip);
    if (id[0] == kSst && id[1] == 0x26) {
        // SST26VF016B/032B/064B: their own size codes.
        if (id[2] < 0x41 || id[2] > 0x43) return fail(Error::NoChip);
        size_ = (2u << 20) << (id[2] - 0x41);
    } else {
        // The others: 2^n bytes, here between 128 KB and 2 GB.
        if (id[2] < 0x11 || id[2] > 0x1F) return fail(Error::NoChip);
        size_ = 1u << id[2];
    }
    fourByte_ = size_ > (1u << 24);

    uint8_t sr = 0;
    if (!waitReady(cfg_.eraseTimeoutMs) || !readStatus(sr)) return false;
    if (id[0] == kSst) {
        // SST26: every block powers up write-protected.
        if (!writeEnable() || !command(kGlobalUnlock)) return false;
    } else if (sr & kStatusProtect) {
        const uint8_t clear[2] = {kWriteStatus, 0x00};
        if (!writeEnable() || !transfer(true, clear, 2, nullptr, 0) || !waitReady(cfg_.programTimeoutMs * 10)) {
            return false;
        }
        if (!readStatus(sr)) return false;
        if (sr & kStatusProtect) return fail(Error::WriteProtected);   // status register locked
    }
    return true;
}

bool SpiNorFlash::read(uint32_t addr, void* buf, size_t len) {
    if (size_ == 0) return fail(Error::NoChip);
    if (addr >= size_ || len > size_ - addr) return fail(Error::Range);
    uint8_t* p = static_cast<uint8_t*>(buf);
    while (len) {
        const size_t n = len < iBlockTransport::kMaxDataLen ? len : iBlockTransport::kMaxDataLen;
        uint8_t h[5];
        if (!transfer(false, h, header(kRead, kRead4, addr, h), p, n)) return false;
        addr += static_cast<uint32_t>(n);
        p += n;
        len -= n;
    }
    return true;
}

bool SpiNorFlash::program(uint32_t addr, const void* data, size_t len) {
    if (size_ == 0) return fail(Error::NoChip);
    if (addr >= size_ || len > size_ - addr) return fail(Error::Range);
    const uint8_t* p = static_cast<const uint8_t*>(data);
    while (len) {
        // Within one page: a program that runs past a page's end wraps to
        // its start on the chip.
        const uint32_t room = kPageBytes - (addr % kPageBytes);
        const size_t n = len < room ? len : room;
        uint8_t h[5];
        if (!writeEnable() || !transfer(true, h, header(kProgram, kProgram4, addr, h), const_cast<uint8_t*>(p), n) ||
            !waitReady(cfg_.programTimeoutMs)) {
            return false;
        }
        addr += static_cast<uint32_t>(n);
        p += n;
        len -= n;
    }
    return true;
}

bool SpiNorFlash::erase(uint32_t addr) {
    if (size_ == 0) return fail(Error::NoChip);
    if (addr >= size_) return fail(Error::Range);
    uint8_t h[5];
    return writeEnable() && transfer(true, h, header(kErase4K, kErase4K4, addr - addr % kSectorBytes, h), nullptr, 0) &&
           waitReady(cfg_.eraseTimeoutMs);
}
