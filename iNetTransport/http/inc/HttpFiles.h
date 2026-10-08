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

// A route handler serving files from a source:
//
//     static HttpStdioFiles disk("0:/www");
//     static HttpStaticFiles site(disk, &builtIn);   // builtIn: when disk has no such file
//     web.get("/api/tags", ...);                     // routes before the catch-all
//     web.get("/*", HttpStaticFiles::handler, &site);
//
// "/" and any path ending in '/' get "index.html". When the client takes
// gzip and "<path>.gz" exists, that is sent instead (Content-Encoding:
// gzip): a UI can be stored compressed. The type comes from the
// extension. Files are sent with Cache-Control: no-cache, so a changed
// UI shows on the next load. Paths with ".." or '\' get 404.
class HttpStaticFiles {
public:
    explicit HttpStaticFiles(HttpFileSource& source, HttpFileSource* fallback = nullptr)
        : source_(source), fallback_(fallback) {}

    static void handler(const HttpRequest& req, HttpResponse& res, void* self);

    static const char* contentType(const char* path);
    static bool        safePath(const char* path);

    static constexpr size_t kMaxPath = 96;

private:
    bool serve(const HttpRequest& req, HttpResponse& res, const char* path) const;

    HttpFileSource& source_;
    HttpFileSource* fallback_;
};
