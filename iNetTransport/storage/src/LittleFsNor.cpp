#include "LittleFsNor.h"
#include <cstring>

LittleFsNor::LittleFsNor(SpiNorFlash& flash, const Config& cfg) : flash_(flash), cfg_(cfg) {
    std::memset(&lfs_, 0, sizeof lfs_);
    std::memset(&lcfg_, 0, sizeof lcfg_);
    lcfg_.context = this;
    lcfg_.read = read;
    lcfg_.prog = prog;
    lcfg_.erase = erase;
    lcfg_.sync = sync;
    lcfg_.read_size = 16;
    lcfg_.prog_size = 16;
    lcfg_.block_size = SpiNorFlash::kSectorBytes;
    lcfg_.cache_size = kCacheBytes;
    lcfg_.lookahead_size = kLookaheadBytes;
    lcfg_.block_cycles = cfg_.blockCycles;
    lcfg_.read_buffer = readBuf_;
    lcfg_.prog_buffer = progBuf_;
    lcfg_.lookahead_buffer = lookahead_;
}

int LittleFsNor::read(const struct lfs_config* c, lfs_block_t block, lfs_off_t off, void* buf, lfs_size_t size) {
    LittleFsNor& me = *static_cast<LittleFsNor*>(c->context);
    const uint32_t addr = (me.cfg_.firstSector + block) * SpiNorFlash::kSectorBytes + off;
    return me.flash_.read(addr, buf, size) ? 0 : LFS_ERR_IO;
}

int LittleFsNor::prog(const struct lfs_config* c, lfs_block_t block, lfs_off_t off, const void* buf, lfs_size_t size) {
    LittleFsNor& me = *static_cast<LittleFsNor*>(c->context);
    const uint32_t addr = (me.cfg_.firstSector + block) * SpiNorFlash::kSectorBytes + off;
    ++me.programs_;
    return me.flash_.program(addr, buf, size) ? 0 : LFS_ERR_IO;
}

int LittleFsNor::erase(const struct lfs_config* c, lfs_block_t block) {
    LittleFsNor& me = *static_cast<LittleFsNor*>(c->context);
    ++me.erases_;
    return me.flash_.erase((me.cfg_.firstSector + block) * SpiNorFlash::kSectorBytes) ? 0 : LFS_ERR_IO;
}

int LittleFsNor::sync(const struct lfs_config*) {
    return 0;   // every program has finished by the time it returns
}

bool LittleFsNor::mount() {
    iLockGuard g(cfg_.lock);
    if (mounted_) return true;
    const uint32_t sectors = flash_.size() / SpiNorFlash::kSectorBytes;
    if (sectors == 0 || cfg_.firstSector >= sectors) {
        error_ = LFS_ERR_INVAL;
        return false;
    }
    lcfg_.block_count = cfg_.sectors ? cfg_.sectors : sectors - cfg_.firstSector;
    if (cfg_.firstSector + lcfg_.block_count > sectors) {
        error_ = LFS_ERR_INVAL;
        return false;
    }
    error_ = lfs_mount(&lfs_, &lcfg_);
    if (error_ != 0 && cfg_.formatIfNeeded) {
        error_ = lfs_format(&lfs_, &lcfg_);
        if (error_ == 0) error_ = lfs_mount(&lfs_, &lcfg_);
    }
    mounted_ = error_ == 0;
    return mounted_;
}

bool LittleFsNor::format() {
    iLockGuard g(cfg_.lock);
    if (mounted_) {
        lfs_unmount(&lfs_);
        mounted_ = false;
    }
    if (lcfg_.block_count == 0) {
        lcfg_.block_count = cfg_.sectors ? cfg_.sectors
                                         : flash_.size() / SpiNorFlash::kSectorBytes - cfg_.firstSector;
    }
    error_ = lfs_format(&lfs_, &lcfg_);
    if (error_ == 0) error_ = lfs_mount(&lfs_, &lcfg_);
    mounted_ = error_ == 0;
    return mounted_;
}

void LittleFsNor::unmount() {
    iLockGuard g(cfg_.lock);
    if (mounted_) lfs_unmount(&lfs_);
    mounted_ = false;
}
