#pragma once
#include <cstddef>
#include <cstdint>
#include "HttpFiles.h"
#include "WebAuth.h"

// Managing the web server's files from a browser, so the UI on SPI flash
// or an SD card can be changed without touching the device:
//
//   GET    /api/files                    {"files":[{"path":"/index.html","size":1234}, ...],
//                                         "total":..., "free":..., "piece":1024}
//   PUT    /api/files/<path>?offset=N    the next piece of an upload (the body: raw bytes)
//   POST   /api/files/<path>?size=N      the upload, N bytes in all, becomes <path>
//   DELETE /api/files/<path>             removes it
//
// A file goes up in pieces of at most `piece` bytes, so each request fits
// the server's Config::requestBytes; the page (httpFileAdminPage,
// "/files.html") does that slicing. Until the commit, the old file is
// the one served. If the file is being sent to a client at that moment,
// the commit (or a delete) waits: with Config::sleep, here, up to
// busyWaitMs, the file held closed to new readers meanwhile (they get the
// next source's copy, such as the built-in page); otherwise, or when that
// runs out, the answer is 503 with Retry-After: 1 and the page tries again.
//
// Every call needs a WebAuth session with Config::role (admin by default),
// and the changes need its CSRF token too. Paths: as HttpStaticFiles
// serves them (no "..", no hidden parts), no control characters, at most
// HttpStaticFiles::kMaxPath. Every change goes to Config::audit.
class HttpFileAdmin {
public:
    typedef void (*AuditFn)(const char* user, const char* action, const char* path, size_t size, void* ctx);

    struct Config {
        WebAuth*  auth = nullptr;                  // required: without it, attach() adds nothing
        WebRole   role = WebRole::Admin;
        size_t    maxFileBytes = 512u * 1024u;
        size_t    pieceBytes = 1024;               // what the page sends per PUT; under requestBytes
        AuditFn   audit = nullptr;
        void*     auditCtx = nullptr;
        // Sleeps the handler's thread (osDelay, vTaskDelay). nullptr: no waiting.
        void      (*sleep)(uint32_t ms, void* ctx) = nullptr;
        void*     sleepCtx = nullptr;
        uint32_t  busyWaitMs = 5000;
    };

    HttpFileAdmin(HttpFileStore& store, const Config& cfg) : store_(store), cfg_(cfg) {}

    template <typename Server>
    bool attach(Server& server) {
        if (cfg_.auth == nullptr) return false;
        return server.on(HttpMethod::Get, "/api/files", list, this) &&
               server.on(HttpMethod::Put, "/api/files/*", put, this) &&
               server.on(HttpMethod::Post, "/api/files/*", commit, this) &&
               server.on(HttpMethod::Delete, "/api/files/*", remove, this);
    }

    static void list(const HttpRequest& req, HttpResponse& res, void* self);
    static void put(const HttpRequest& req, HttpResponse& res, void* self);
    static void commit(const HttpRequest& req, HttpResponse& res, void* self);
    static void remove(const HttpRequest& req, HttpResponse& res, void* self);

    // The path a request names, checked. nullptr: not a file path we take.
    static const char* filePath(const HttpRequest& req);

private:
    bool queryNumber(const HttpRequest& req, const char* name, size_t& out) const;
    enum class Done : uint8_t { Ok, Failed, Busy };
    // commit (size) or remove (remove), waiting for the file's readers.
    Done change(const char* path, bool remove, size_t size);

    HttpFileStore& store_;
    Config         cfg_;
};

// The page for it: list, upload (with progress), delete, behind a login.
// Add it to the firmware's built-in files:
//     static const HttpMemoryFiles::File builtIn[] = { ..., httpFileAdminPage };
extern const HttpMemoryFiles::File httpFileAdminPage;
