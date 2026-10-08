#pragma once
#include <cstddef>
#include <cstdint>
#include "HttpFiles.h"
#include "LittleFsNor.h"

// The web server's files on LittleFS (SPI flash): served
// (HttpFileSource) and uploaded (HttpFileStore, for HttpFileAdmin).
//
//     static HttpLittleFsFiles flashFiles(fs, "/www");     // under that folder
//     static HttpStaticFiles site(card, &flashFiles, &builtIn);
//
// An upload goes into a hidden file beside its target, appended to piece
// by piece; commit() renames it over the target, which LittleFS does in
// one step, so a power cut leaves either the old file or the new one.
// Up to kMaxOpen files are open for reading at once, each with its own
// 256-byte buffer here. A file open for reading isn't replaced or removed
// (busy()); writeFile() returns false then too.
class HttpLittleFsFiles : public HttpFileSource, public HttpFileStore {
public:
    static constexpr uint8_t kMaxOpen = 4;
    static constexpr size_t  kMaxFullPath = HttpStaticFiles::kMaxPath + 32;

    HttpLittleFsFiles(LittleFsNor& fs, const char* root = "/www");

    // HttpFileSource
    void*  open(const char* path, size_t& size) override;
    size_t read(void* file, size_t offset, uint8_t* buf, size_t len) override;
    void   close(void* file) override;

    // HttpFileStore
    bool append(const char* path, size_t offset, const uint8_t* data, size_t len) override;
    bool commit(const char* path, size_t size) override;
    bool remove(const char* path) override;
    bool list(ListFn fn, void* ctx) override;
    bool space(uint64_t& total, uint64_t& free) override;
    bool busy(const char* path) override;
    void hold(const char* path, bool on) override;

    // Writes a whole file (replacing it), the same way: for firmware that
    // puts its own pages in place.
    bool writeFile(const char* path, const uint8_t* data, size_t len);

private:
    struct Slot {
        bool                   used;
        uint32_t               hash;   // of the full path
        lfs_file_t             file;
        struct lfs_file_config cfg;
        alignas(4) uint8_t     buf[LittleFsNor::kCacheBytes];
    };

    bool full(const char* path, char* out, size_t cap) const;
    bool isOpen(const char* full) const;   // under the lock
    bool makeParents(const char* full);
    bool walk(char* dir, size_t len, int depth, ListFn fn, void* ctx);

    LittleFsNor& fs_;
    const char*  root_;
    uint32_t     held_ = 0;   // pathHash() of the path hold() keeps closed
    Slot         slots_[kMaxOpen] = {};
    Slot         writer_ = {};   // uploads: one at a time, under the lock
};
