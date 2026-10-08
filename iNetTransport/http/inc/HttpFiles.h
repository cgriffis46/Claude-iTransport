#pragma once
#include <cstddef>
#include <cstdint>
#include "HttpConnection.h"

// Where a web server's files come from, so the pages can change without
// the firmware: an SD card or SPI flash behind a file system, a folder
// on Linux, or (as the fallback, or for a fixed UI) arrays in flash.
// Paths start with '/' and never contain ".." (HttpStaticFiles checks).
// Reads are by offset, so one source can serve several clients at once,
// each from its own thread.
class HttpFileSource {
public:
    virtual ~HttpFileSource() = default;
    // The file at path: a handle, and its size. nullptr: there is none.
    virtual void*  open(const char* path, size_t& size) = 0;
    // Up to len bytes from offset. 0: the end, or an error.
    virtual size_t read(void* file, size_t offset, uint8_t* buf, size_t len) = 0;
    virtual void   close(void* file) = 0;
};

// Where uploaded files go (HttpFileAdmin): a file system that can be
// written, such as LittleFS on SPI flash or FatFs on an SD card. An
// upload arrives in pieces, over several requests, into a hidden file
// beside the target ("/.index.html.part" for "/index.html"); commit()
// then puts it in place in one step, so the old file is served until the
// new one is complete.
class HttpFileStore {
public:
    virtual ~HttpFileStore() = default;
    // Adds len bytes at offset to path's upload. offset 0 starts it
    // afresh. false: offset isn't where the upload has got to, or the
    // file system is full or failed.
    virtual bool append(const char* path, size_t offset, const uint8_t* data, size_t len) = 0;
    // The upload becomes path, replacing any file there, if it holds
    // exactly size bytes. false: it doesn't, or there is no upload.
    // Both refuse a path that is open for reading (busy()): a client
    // part way through the old file would otherwise read blocks the file
    // system has given to something else.
    virtual bool commit(const char* path, size_t size) = 0;
    virtual bool remove(const char* path) = 0;
    // true: path is being read now; commit() and remove() wait for that.
    virtual bool busy(const char* path) { (void)path; return false; }
    // While held, path isn't opened for reading (a server falls through to
    // its next source), so the readers it has drain away and a commit can
    // go through even when the file is fetched all the time. One path at a
    // time; hold(path, false) lets it be read again.
    virtual void hold(const char* path, bool on) { (void)path; (void)on; }
    // Every file, as "/dir/name", with its size. Hidden files (a name
    // starting with '.') are left out.
    typedef void (*ListFn)(const char* path, size_t size, void* ctx);
    virtual bool list(ListFn fn, void* ctx) = 0;
    // Bytes in all, and free.
    virtual bool space(uint64_t& total, uint64_t& free) = 0;

    // "/dir/name" -> "/dir/.name.part". false: too long for cap.
    static bool uploadPath(const char* path, char* out, size_t cap);
    // FNV-1a of a path, never 0: how the stores remember what's open.
    static uint32_t pathHash(const char* path);
};

// Files compiled into the firmware: a table of paths and their bytes.
class HttpMemoryFiles : public HttpFileSource {
public:
    struct File {
        const char*    path;   // "/index.html"
        const uint8_t* data;
        size_t         size;
    };
    HttpMemoryFiles(const File* files, size_t count) : files_(files), count_(count) {}

    void*  open(const char* path, size_t& size) override;
    size_t read(void* file, size_t offset, uint8_t* buf, size_t len) override;
    void   close(void*) override {}

private:
    const File* files_;
    size_t      count_;
};

// Files under a folder, through the C library's FILE*: on Linux, or on
// an MCU whose newlib calls (_open, _read, ...) reach a file system such
// as FatFs. root: "/var/www" or "0:/www", without a trailing '/'.
class HttpStdioFiles : public HttpFileSource {
public:
    explicit HttpStdioFiles(const char* root) : root_(root) {}

    void*  open(const char* path, size_t& size) override;
    size_t read(void* file, size_t offset, uint8_t* buf, size_t len) override;
    void   close(void* file) override;

private:
    const char* root_;
};

// A route handler serving files from up to three sources, tried in
// order: an SD card, then SPI flash, then the pages built into the
// firmware, say.
//
//     static HttpFatFsFiles card("0:/www");
//     static HttpLittleFsFiles flash(lfs);
//     static HttpMemoryFiles builtIn(files, n);
//     static HttpStaticFiles site(card, &flash, &builtIn);
//     web.get("/api/tags", ...);                     // routes before the catch-all
//     web.get("/*", HttpStaticFiles::handler, &site);
//
// "/" and any path ending in '/' get "index.html". When the client takes
// gzip and "<path>.gz" exists, that is sent instead (Content-Encoding:
// gzip): a UI can be stored compressed. The type comes from the
// extension. Files are sent with Cache-Control: no-cache, so a changed
// UI shows on the next load. Paths with "..", '\', or a part starting
// with '.' (hidden files, such as uploads in progress) get 404.
class HttpStaticFiles {
public:
    explicit HttpStaticFiles(HttpFileSource& first, HttpFileSource* second = nullptr,
                             HttpFileSource* third = nullptr)
        : sources_{&first, second, third} {}

    static void handler(const HttpRequest& req, HttpResponse& res, void* self);

    static const char* contentType(const char* path);
    static bool        safePath(const char* path);

    static constexpr size_t kMaxPath = 96;

private:
    bool serve(const HttpRequest& req, HttpResponse& res, const char* path) const;

    HttpFileSource* sources_[3];
};
