#pragma once
#include <cstddef>
#include <cstdint>
#include "HttpFiles.h"
#include "iLock.h"
#include "ff.h"

// The web server's files on a FatFs volume (an SD card through the
// F207's SDIO, as CubeMX sets up FATFS): served (HttpFileSource) and
// uploaded (HttpFileStore, for HttpFileAdmin).
//
//     static HttpFatFsFiles card("0:/www", &cardLock);     // f_mount()ed by the application
//     static HttpStaticFiles site(card, &flashFiles, &builtIn);
//
// Long file names are needed for names such as "index.html" or
// "app.js.gz": set FF_USE_LFN to 2 or 3 (CubeMX: USE_LFN "Enabled with
// dynamic working buffer on the STACK/HEAP"). 1 uses one static buffer,
// which threads would share.
//
// lock: serialises this class's calls. If other code uses the card too,
// either give it the same lock or build FatFs with FF_FS_REENTRANT 1.
//
// Uploads go to a hidden file beside the target. FatFs can't rename over
// a file, so commit() removes the old file first: for that moment there
// is none, and a power cut then leaves only the upload (as
// ".name.part"). LittleFS on SPI flash doesn't have that gap.
//
// Up to kMaxOpen files are open for reading at once; each FIL here holds
// a 512-byte sector buffer unless FF_FS_TINY is 1. A file open for
// reading isn't replaced or removed (busy()); writeFile() returns false
// then too.
class HttpFatFsFiles : public HttpFileSource, public HttpFileStore {
public:
    static constexpr uint8_t kMaxOpen = 4;
    static constexpr size_t  kMaxFullPath = HttpStaticFiles::kMaxPath + 32;

    explicit HttpFatFsFiles(const char* root = "0:/www", iLock* lock = nullptr);

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

    bool writeFile(const char* path, const uint8_t* data, size_t len);

private:
    struct Slot {
        bool     used;
        uint32_t hash;   // of the full path
        FIL      file;
    };

    bool full(const char* path, char* out, size_t cap) const;
    bool isOpen(const char* full) const;   // under the lock
    bool makeParents(const char* full);
    bool walk(char* dir, size_t len, int depth, ListFn fn, void* ctx);

    const char* root_;
    iLock*      lock_;
    uint32_t    held_ = 0;   // pathHash() of the path hold() keeps closed
    Slot        slots_[kMaxOpen] = {};
    FIL         writer_ = {};
    DIR         dirs_[9] = {};   // one per level while listing
};
