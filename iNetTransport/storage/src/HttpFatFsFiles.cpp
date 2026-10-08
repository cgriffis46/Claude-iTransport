#include "HttpFatFsFiles.h"
#include <cstdio>
#include <cstring>

#if !FF_USE_LFN
#warning "FatFs is built without long file names (FF_USE_LFN 0): only 8.3 names such as INDEX.HTM can be served"
#endif

HttpFatFsFiles::HttpFatFsFiles(const char* root, iLock* lock) : root_(root ? root : ""), lock_(lock) {}

bool HttpFatFsFiles::full(const char* path, char* out, size_t cap) const {
    const int n = std::snprintf(out, cap, "%s%s", root_, path);
    return n > 0 && static_cast<size_t>(n) < cap;
}

void* HttpFatFsFiles::open(const char* path, size_t& size) {
    char p[kMaxFullPath];
    if (!full(path, p, sizeof p)) return nullptr;
    iLockGuard g(lock_);
    if (held_ != 0 && held_ == pathHash(p)) return nullptr;   // being replaced
    for (Slot& s : slots_) {
        if (s.used) continue;
        if (f_open(&s.file, p, FA_READ) != FR_OK) return nullptr;
        size = static_cast<size_t>(f_size(&s.file));
        s.used = true;
        s.hash = pathHash(p);
        return &s;
    }
    return nullptr;
}

size_t HttpFatFsFiles::read(void* file, size_t offset, uint8_t* buf, size_t len) {
    Slot& s = *static_cast<Slot*>(file);
    iLockGuard g(lock_);
    if (f_tell(&s.file) != offset && f_lseek(&s.file, offset) != FR_OK) return 0;
    UINT got = 0;
    if (f_read(&s.file, buf, static_cast<UINT>(len), &got) != FR_OK) return 0;
    return got;
}

void HttpFatFsFiles::close(void* file) {
    Slot& s = *static_cast<Slot*>(file);
    iLockGuard g(lock_);
    f_close(&s.file);
    s.used = false;
}

// Every folder above full, made if missing ("0:/www/a/b.txt": 0:/www, 0:/www/a).
bool HttpFatFsFiles::makeParents(const char* full) {
    char dir[kMaxFullPath];
    std::strncpy(dir, full, sizeof dir - 1);
    dir[sizeof dir - 1] = 0;
    const char* start = std::strchr(dir, ':');
    char* p = start ? const_cast<char*>(start) + 2 : dir + 1;   // past "0:/", or the leading '/'
    for (; (p = std::strchr(p, '/')) != nullptr; ++p) {
        *p = 0;
        const FRESULT r = f_mkdir(dir);
        *p = '/';
        if (r != FR_OK && r != FR_EXIST) return false;
    }
    return true;
}

bool HttpFatFsFiles::append(const char* path, size_t offset, const uint8_t* data, size_t len) {
    char target[kMaxFullPath], part[kMaxFullPath + 8];
    if (!full(path, target, sizeof target) || !uploadPath(target, part, sizeof part)) return false;
    iLockGuard g(lock_);
    if (offset == 0 && !makeParents(target)) return false;
    const BYTE mode = offset == 0 ? (FA_WRITE | FA_CREATE_ALWAYS) : (FA_WRITE | FA_OPEN_APPEND);
    if (f_open(&writer_, part, mode) != FR_OK) return false;
    bool ok = f_size(&writer_) == offset;
    UINT done = 0;
    if (ok && len) ok = f_write(&writer_, data, static_cast<UINT>(len), &done) == FR_OK && done == len;
    if (f_close(&writer_) != FR_OK) ok = false;   // flushes it to the card
    return ok;
}

bool HttpFatFsFiles::commit(const char* path, size_t size) {
    char target[kMaxFullPath], part[kMaxFullPath + 8];
    if (!full(path, target, sizeof target) || !uploadPath(target, part, sizeof part)) return false;
    iLockGuard g(lock_);
    FILINFO info;
    if (isOpen(target) || f_stat(part, &info) != FR_OK || (info.fattrib & AM_DIR) || info.fsize != size) return false;
    const FRESULT r = f_unlink(target);
    if (r != FR_OK && r != FR_NO_FILE) return false;
    // f_rename takes the new name without the drive.
    const char* newName = std::strchr(target, ':') ? std::strchr(target, ':') + 1 : target;
    return f_rename(part, newName) == FR_OK;
}

bool HttpFatFsFiles::writeFile(const char* path, const uint8_t* data, size_t len) {
    return append(path, 0, data, len) && commit(path, len);
}

bool HttpFatFsFiles::remove(const char* path) {
    char target[kMaxFullPath];
    if (!full(path, target, sizeof target)) return false;
    iLockGuard g(lock_);
    FILINFO info;
    if (isOpen(target) || f_stat(target, &info) != FR_OK || (info.fattrib & AM_DIR)) return false;
    return f_unlink(target) == FR_OK;
}

bool HttpFatFsFiles::walk(char* dir, size_t len, int depth, ListFn fn, void* ctx) {
    if (depth >= static_cast<int>(sizeof dirs_ / sizeof dirs_[0])) return true;
    DIR& d = dirs_[depth];
    if (f_opendir(&d, dir) != FR_OK) return false;
    FILINFO info;
    const size_t rootLen = std::strlen(root_);
    while (f_readdir(&d, &info) == FR_OK && info.fname[0] != 0) {
        if (info.fname[0] == '.' || (info.fattrib & (AM_HID | AM_SYS))) continue;
        const size_t n = std::strlen(info.fname);
        if (len + 1 + n >= kMaxFullPath) continue;
        dir[len] = '/';
        std::memcpy(dir + len + 1, info.fname, n + 1);
        if (info.fattrib & AM_DIR) walk(dir, len + 1 + n, depth + 1, fn, ctx);
        else fn(dir + rootLen, static_cast<size_t>(info.fsize), ctx);
        dir[len] = 0;
    }
    f_closedir(&d);
    return true;
}

bool HttpFatFsFiles::list(ListFn fn, void* ctx) {
    char dir[kMaxFullPath];
    std::strncpy(dir, root_, sizeof dir - 1);
    dir[sizeof dir - 1] = 0;
    iLockGuard g(lock_);
    FILINFO info;
    if (f_stat(dir, &info) != FR_OK) return true;   // no folder yet: no files
    return walk(dir, std::strlen(dir), 0, fn, ctx);
}

bool HttpFatFsFiles::space(uint64_t& total, uint64_t& free) {
    char drive[8] = "";
    const char* colon = std::strchr(root_, ':');
    if (colon && static_cast<size_t>(colon - root_) + 2 < sizeof drive) {
        std::memcpy(drive, root_, static_cast<size_t>(colon - root_) + 1);
        drive[colon - root_ + 1] = 0;
    }
    iLockGuard g(lock_);
    FATFS* fs = nullptr;
    DWORD clusters = 0;
    if (f_getfree(drive, &clusters, &fs) != FR_OK || fs == nullptr) return false;
#if FF_MAX_SS != FF_MIN_SS
    const uint64_t sector = fs->ssize;
#else
    const uint64_t sector = FF_MAX_SS;
#endif
    total = static_cast<uint64_t>(fs->n_fatent - 2) * fs->csize * sector;
    free = static_cast<uint64_t>(clusters) * fs->csize * sector;
    return true;
}

// A file being read keeps its blocks only while it has a name: replacing
// or removing it under a reader would hand them to the next write.
bool HttpFatFsFiles::isOpen(const char* full) const {
    const uint32_t h = pathHash(full);
    for (const Slot& s : slots_) {
        if (s.used && s.hash == h) return true;
    }
    return false;
}

bool HttpFatFsFiles::busy(const char* path) {
    char target[kMaxFullPath];
    if (!full(path, target, sizeof target)) return false;
    iLockGuard g(lock_);
    return isOpen(target);
}

void HttpFatFsFiles::hold(const char* path, bool on) {
    char target[kMaxFullPath];
    if (!full(path, target, sizeof target)) return;
    const uint32_t h = pathHash(target);
    iLockGuard g(lock_);
    if (on) held_ = h;
    else if (held_ == h) held_ = 0;
}
