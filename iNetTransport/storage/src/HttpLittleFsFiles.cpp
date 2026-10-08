#include "HttpLittleFsFiles.h"
#include <cstdio>
#include <cstring>

HttpLittleFsFiles::HttpLittleFsFiles(LittleFsNor& fs, const char* root) : fs_(fs), root_(root ? root : "") {}

// root + path, in out.
bool HttpLittleFsFiles::full(const char* path, char* out, size_t cap) const {
    const int n = std::snprintf(out, cap, "%s%s", root_, path);
    return n > 0 && static_cast<size_t>(n) < cap;
}

void* HttpLittleFsFiles::open(const char* path, size_t& size) {
    char p[kMaxFullPath];
    if (!full(path, p, sizeof p)) return nullptr;
    iLockGuard g(fs_.lock());
    if (!fs_.mounted() || (held_ != 0 && held_ == pathHash(p))) return nullptr;   // held: being replaced
    for (Slot& s : slots_) {
        if (s.used) continue;
        std::memset(&s.cfg, 0, sizeof s.cfg);
        s.cfg.buffer = s.buf;
        if (lfs_file_opencfg(fs_.lfs(), &s.file, p, LFS_O_RDONLY, &s.cfg) != 0) return nullptr;
        const lfs_soff_t n = lfs_file_size(fs_.lfs(), &s.file);
        if (n < 0) {
            lfs_file_close(fs_.lfs(), &s.file);
            return nullptr;
        }
        size = static_cast<size_t>(n);
        s.used = true;
        s.hash = pathHash(p);
        return &s;
    }
    return nullptr;   // all slots busy: the server answers 404, and the browser tries again later
}

size_t HttpLittleFsFiles::read(void* file, size_t offset, uint8_t* buf, size_t len) {
    Slot& s = *static_cast<Slot*>(file);
    iLockGuard g(fs_.lock());
    if (lfs_file_seek(fs_.lfs(), &s.file, static_cast<lfs_soff_t>(offset), LFS_SEEK_SET) < 0) return 0;
    const lfs_ssize_t n = lfs_file_read(fs_.lfs(), &s.file, buf, static_cast<lfs_size_t>(len));
    return n > 0 ? static_cast<size_t>(n) : 0;
}

void HttpLittleFsFiles::close(void* file) {
    Slot& s = *static_cast<Slot*>(file);
    iLockGuard g(fs_.lock());
    lfs_file_close(fs_.lfs(), &s.file);
    s.used = false;
}

// Every folder above full, made if missing.
bool HttpLittleFsFiles::makeParents(const char* full) {
    char dir[kMaxFullPath];
    std::strncpy(dir, full, sizeof dir - 1);
    dir[sizeof dir - 1] = 0;
    for (char* p = dir + 1; (p = std::strchr(p, '/')) != nullptr; ++p) {
        *p = 0;
        const int r = lfs_mkdir(fs_.lfs(), dir);
        *p = '/';
        if (r != 0 && r != LFS_ERR_EXIST) return false;
    }
    return true;
}

bool HttpLittleFsFiles::append(const char* path, size_t offset, const uint8_t* data, size_t len) {
    char target[kMaxFullPath], part[kMaxFullPath + 8];
    if (!full(path, target, sizeof target) || !uploadPath(target, part, sizeof part)) return false;
    iLockGuard g(fs_.lock());
    if (!fs_.mounted() || (offset == 0 && !makeParents(target))) return false;
    std::memset(&writer_.cfg, 0, sizeof writer_.cfg);
    writer_.cfg.buffer = writer_.buf;
    const int flags = offset == 0 ? (LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC) : (LFS_O_WRONLY | LFS_O_APPEND);
    if (lfs_file_opencfg(fs_.lfs(), &writer_.file, part, flags, &writer_.cfg) != 0) return false;
    bool ok = lfs_file_size(fs_.lfs(), &writer_.file) == static_cast<lfs_soff_t>(offset);
    if (ok && len) ok = lfs_file_write(fs_.lfs(), &writer_.file, data, static_cast<lfs_size_t>(len)) == static_cast<lfs_ssize_t>(len);
    // close() commits: the piece is on the flash when this returns.
    if (lfs_file_close(fs_.lfs(), &writer_.file) != 0) ok = false;
    return ok;
}

bool HttpLittleFsFiles::commit(const char* path, size_t size) {
    char target[kMaxFullPath], part[kMaxFullPath + 8];
    if (!full(path, target, sizeof target) || !uploadPath(target, part, sizeof part)) return false;
    iLockGuard g(fs_.lock());
    struct lfs_info info;
    if (!fs_.mounted() || isOpen(target) || lfs_stat(fs_.lfs(), part, &info) != 0 || info.type != LFS_TYPE_REG ||
        info.size != size) {
        return false;
    }
    return lfs_rename(fs_.lfs(), part, target) == 0;   // replaces target in one step
}

bool HttpLittleFsFiles::writeFile(const char* path, const uint8_t* data, size_t len) {
    return append(path, 0, data, len) && commit(path, len);
}

bool HttpLittleFsFiles::remove(const char* path) {
    char target[kMaxFullPath];
    if (!full(path, target, sizeof target)) return false;
    iLockGuard g(fs_.lock());
    struct lfs_info info;
    if (!fs_.mounted() || isOpen(target) || lfs_stat(fs_.lfs(), target, &info) != 0 || info.type != LFS_TYPE_REG) {
        return false;
    }
    return lfs_remove(fs_.lfs(), target) == 0;
}

// dir holds len characters of a folder's full path; the files under it go
// to fn, as paths below root.
bool HttpLittleFsFiles::walk(char* dir, size_t len, int depth, ListFn fn, void* ctx) {
    if (depth > 8) return true;
    lfs_dir_t d;
    if (lfs_dir_open(fs_.lfs(), &d, len ? dir : "/") != 0) return false;
    struct lfs_info info;
    const size_t rootLen = std::strlen(root_);
    while (lfs_dir_read(fs_.lfs(), &d, &info) > 0) {
        if (info.name[0] == '.') continue;   // ".", "..", and hidden files (uploads)
        const size_t n = std::strlen(info.name);
        if (len + 1 + n >= kMaxFullPath) continue;
        dir[len] = '/';
        std::memcpy(dir + len + 1, info.name, n + 1);
        if (info.type == LFS_TYPE_DIR) walk(dir, len + 1 + n, depth + 1, fn, ctx);
        else fn(dir + rootLen, info.size, ctx);
        dir[len] = 0;
    }
    lfs_dir_close(fs_.lfs(), &d);
    return true;
}

bool HttpLittleFsFiles::list(ListFn fn, void* ctx) {
    char dir[kMaxFullPath];
    std::strncpy(dir, root_, sizeof dir - 1);
    dir[sizeof dir - 1] = 0;
    iLockGuard g(fs_.lock());
    if (!fs_.mounted()) return false;
    struct lfs_info info;
    if (dir[0] && lfs_stat(fs_.lfs(), dir, &info) != 0) return true;   // no folder yet: no files
    return walk(dir, std::strlen(dir), 0, fn, ctx);
}

bool HttpLittleFsFiles::space(uint64_t& total, uint64_t& free) {
    iLockGuard g(fs_.lock());
    if (!fs_.mounted()) return false;
    total = static_cast<uint64_t>(fs_.blockCount()) * fs_.blockSize();
    const lfs_ssize_t used = lfs_fs_size(fs_.lfs());
    if (used < 0) return false;
    free = total - static_cast<uint64_t>(used) * fs_.blockSize();
    return true;
}

// A file being read keeps its blocks only while it has a name: replacing
// or removing it under a reader would hand them to the next write.
bool HttpLittleFsFiles::isOpen(const char* full) const {
    const uint32_t h = pathHash(full);
    for (const Slot& s : slots_) {
        if (s.used && s.hash == h) return true;
    }
    return false;
}

bool HttpLittleFsFiles::busy(const char* path) {
    char target[kMaxFullPath];
    if (!full(path, target, sizeof target)) return false;
    iLockGuard g(fs_.lock());
    return isOpen(target);
}

void HttpLittleFsFiles::hold(const char* path, bool on) {
    char target[kMaxFullPath];
    if (!full(path, target, sizeof target)) return;
    const uint32_t h = pathHash(target);
    iLockGuard g(fs_.lock());
    if (on) held_ = h;
    else if (held_ == h) held_ = 0;
}
