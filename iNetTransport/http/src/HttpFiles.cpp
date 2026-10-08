#include "HttpFiles.h"
#include <cstdio>
#include <cstring>

namespace {

bool endsWithNoCase(const char* s, const char* suffix) {
    const size_t n = std::strlen(s), m = std::strlen(suffix);
    if (m > n) return false;
    for (size_t i = 0; i < m; ++i) {
        if ((s[n - m + i] | 0x20) != (suffix[i] | 0x20)) return false;
    }
    return true;
}

// Whether a comma-separated header value (Accept-Encoding) has this
// token, ignoring parameters (";q=..."). q=0 still counts: rare enough.
bool acceptsGzip(const char* list) {
    if (list == nullptr) return false;
    for (const char* p = list; *p;) {
        while (*p == ' ' || *p == '\t' || *p == ',') ++p;
        const char* e = p;
        while (*e && *e != ',' && *e != ';' && *e != ' ') ++e;
        if (e - p == 4 && (p[0] | 0x20) == 'g' && (p[1] | 0x20) == 'z' && (p[2] | 0x20) == 'i' && (p[3] | 0x20) == 'p') {
            return true;
        }
        while (*e && *e != ',') ++e;
        p = e;
    }
    return false;
}

}  // namespace

// ---- HttpMemoryFiles ----

void* HttpMemoryFiles::open(const char* path, size_t& size) {
    for (size_t i = 0; i < count_; ++i) {
        if (std::strcmp(files_[i].path, path) == 0) {
            size = files_[i].size;
            return const_cast<File*>(&files_[i]);
        }
    }
    return nullptr;
}

size_t HttpMemoryFiles::read(void* file, size_t offset, uint8_t* buf, size_t len) {
    const File* f = static_cast<const File*>(file);
    if (offset >= f->size) return 0;
    const size_t n = len < f->size - offset ? len : f->size - offset;
    std::memcpy(buf, f->data + offset, n);
    return n;
}

// ---- HttpStdioFiles ----

void* HttpStdioFiles::open(const char* path, size_t& size) {
    char full[HttpStaticFiles::kMaxPath + 64];
    const int n = std::snprintf(full, sizeof full, "%s%s", root_, path);
    if (n < 0 || static_cast<size_t>(n) >= sizeof full) return nullptr;
    FILE* f = std::fopen(full, "rb");
    if (f == nullptr) return nullptr;
    if (std::fseek(f, 0, SEEK_END) != 0) { std::fclose(f); return nullptr; }
    const long end = std::ftell(f);
    // A folder opens too on some systems (Linux), but can't be read.
    if (end < 0 || std::fseek(f, 0, SEEK_SET) != 0 || (end > 0 && std::fgetc(f) == EOF) ||
        std::fseek(f, 0, SEEK_SET) != 0) {
        std::fclose(f);
        return nullptr;
    }
    size = static_cast<size_t>(end);
    return f;
}

size_t HttpStdioFiles::read(void* file, size_t offset, uint8_t* buf, size_t len) {
    FILE* f = static_cast<FILE*>(file);
    if (std::ftell(f) != static_cast<long>(offset) && std::fseek(f, static_cast<long>(offset), SEEK_SET) != 0) return 0;
    return std::fread(buf, 1, len, f);
}

void HttpStdioFiles::close(void* file) {
    std::fclose(static_cast<FILE*>(file));
}

// ---- HttpStaticFiles ----

const char* HttpStaticFiles::contentType(const char* path) {
    static const struct { const char* ext; const char* type; } kTypes[] = {
        {".html", "text/html; charset=utf-8"},
        {".htm", "text/html; charset=utf-8"},
        {".css", "text/css; charset=utf-8"},
        {".js", "text/javascript; charset=utf-8"},
        {".json", "application/json"},
        {".svg", "image/svg+xml"},
        {".png", "image/png"},
        {".jpg", "image/jpeg"},
        {".jpeg", "image/jpeg"},
        {".gif", "image/gif"},
        {".ico", "image/x-icon"},
        {".txt", "text/plain; charset=utf-8"},
        {".woff2", "font/woff2"},
    };
    for (const auto& t : kTypes) {
        if (endsWithNoCase(path, t.ext)) return t.type;
    }
    return "application/octet-stream";
}

bool HttpStaticFiles::safePath(const char* path) {
    if (path == nullptr || path[0] != '/' || std::strchr(path, '\\') != nullptr) return false;
    // No ".." segment anywhere: nothing outside the root.
    for (const char* p = path; (p = std::strstr(p, "..")) != nullptr; p += 2) {
        const bool startsSegment = p[-1] == '/';
        const bool endsSegment = p[2] == '/' || p[2] == 0;
        if (startsSegment && endsSegment) return false;
    }
    return true;
}

void HttpStaticFiles::handler(const HttpRequest& req, HttpResponse& res, void* self) {
    const HttpStaticFiles& me = *static_cast<const HttpStaticFiles*>(self);
    const char* p = req.path();
    char path[kMaxPath + 12];
    const size_t n = std::strlen(p);
    if (!safePath(p) || n > kMaxPath) {
        res.status(404).send("text/plain", "Not Found");
        return;
    }
    std::memcpy(path, p, n + 1);
    if (path[n - 1] == '/') std::memcpy(path + n, "index.html", 11);
    if (!me.serve(req, res, path)) res.status(404).send("text/plain", "Not Found");
}

bool HttpStaticFiles::serve(const HttpRequest& req, HttpResponse& res, const char* path) const {
    HttpFileSource* sources[2] = {&source_, fallback_};
    const bool gzipOk = acceptsGzip(req.header("Accept-Encoding"));
    char gz[kMaxPath + 16];
    std::snprintf(gz, sizeof gz, "%s.gz", path);

    for (HttpFileSource* src : sources) {
        if (src == nullptr) continue;
        size_t size = 0;
        bool gzipped = false;
        void* f = gzipOk ? src->open(gz, size) : nullptr;
        if (f) gzipped = true;
        else f = src->open(path, size);
        if (f == nullptr) continue;

        res.header("Cache-Control", "no-cache");
        if (gzipped) res.header("Content-Encoding", "gzip");
        res.header("Vary", "Accept-Encoding");
        res.begin(contentType(path), static_cast<int32_t>(size));
        uint8_t buf[256];
        size_t at = 0;
        while (at < size) {
            const size_t want = size - at < sizeof buf ? size - at : sizeof buf;
            const size_t got = src->read(f, at, buf, want);
            if (got == 0 || !res.write(buf, got)) break;   // short: end() closes the connection
            at += got;
        }
        src->close(f);
        res.end();
        return true;
    }
    return false;
}
