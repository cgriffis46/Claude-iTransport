#pragma once
#include <cstddef>
#include <cstdint>
#include "iLock.h"
#include "lfs.h"
#include "SpiNorFlash.h"

// LittleFS on a SpiNorFlash: a file system that survives a power cut at
// any moment (a file being written is either its old self or its new
// one), and spreads wear over the chip. Its buffers are all here, so it
// never calls malloc (build lfs.c with LFS_NO_MALLOC).
//
//     LittleFsNor::Config c;
//     c.lock = &flashLock;                    // every user of the file system takes it
//     static LittleFsNor fs(flash, c);
//     fs.mount();                             // formats a blank (or foreign) chip first
//
// LittleFS itself isn't thread-safe: every call on lfs() must be made
// holding lock(). HttpLittleFsFiles does.
class LittleFsNor {
public:
    static constexpr uint32_t kCacheBytes = 256;      // = the chip's page
    static constexpr uint32_t kLookaheadBytes = 32;   // 256 blocks per scan

    struct Config {
        iLock*   lock = nullptr;
        uint32_t firstSector = 0;       // the part of the chip to use, in 4 KB sectors
        uint32_t sectors = 0;           // 0: from firstSector to the end
        int32_t  blockCycles = 500;     // erase cycles before LittleFS moves a block's data
        bool     formatIfNeeded = true; // mount() formats when there is no file system
    };

    LittleFsNor(SpiNorFlash& flash, const Config& cfg);

    // After the flash's begin(). false: see error() (an lfs_error), or
    // the flash's.
    bool mount();
    bool format();
    void unmount();

    lfs_t*  lfs() { return &lfs_; }
    iLock*  lock() { return cfg_.lock; }
    bool    mounted() const { return mounted_; }
    int     error() const { return error_; }
    uint32_t blockCount() const { return lcfg_.block_count; }
    uint32_t blockSize() const { return lcfg_.block_size; }

    // How many sectors have been erased and pages programmed: for wear
    // estimates and tests.
    uint32_t erases() const { return erases_; }
    uint32_t programs() const { return programs_; }

private:
    static int read(const struct lfs_config* c, lfs_block_t block, lfs_off_t off, void* buf, lfs_size_t size);
    static int prog(const struct lfs_config* c, lfs_block_t block, lfs_off_t off, const void* buf, lfs_size_t size);
    static int erase(const struct lfs_config* c, lfs_block_t block);
    static int sync(const struct lfs_config* c);

    SpiNorFlash&      flash_;
    Config            cfg_;
    lfs_t             lfs_;
    struct lfs_config lcfg_;
    bool              mounted_ = false;
    int               error_ = 0;
    uint32_t          erases_ = 0, programs_ = 0;
    alignas(4) uint8_t readBuf_[kCacheBytes];
    alignas(4) uint8_t progBuf_[kCacheBytes];
    alignas(4) uint8_t lookahead_[kLookaheadBytes];
};
