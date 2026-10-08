#include "HttpFileAdmin.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

void jsonAnswer(HttpResponse& res, uint16_t status, const char* body) {
    res.status(status).header("Cache-Control", "no-store");
    res.send("application/json", body);
}

// The file is being sent to someone: the page tries again shortly.
void busyAnswer(HttpResponse& res) {
    res.status(503).header("Retry-After", "1").header("Cache-Control", "no-store");
    res.send("application/json", "{\"error\":\"the file is being read; try again\"}");
}

void jsonError(HttpResponse& res, uint16_t status, const char* message) {
    char body[112];
    std::snprintf(body, sizeof body, "{\"error\":\"%s\"}", message);
    jsonAnswer(res, status, body);
}

// A path as a JSON string: quotes, backslashes and anything odd escaped.
void writeJsonString(HttpResponse& res, const char* s) {
    char buf[2 * HttpStaticFiles::kMaxPath + 8];
    size_t n = 0;
    buf[n++] = '"';
    for (const char* p = s; *p && n < sizeof buf - 8; ++p) {
        const unsigned char c = static_cast<unsigned char>(*p);
        if (c == '"' || c == '\\') { buf[n++] = '\\'; buf[n++] = static_cast<char>(c); }
        else if (c < 0x20) n += static_cast<size_t>(std::snprintf(buf + n, sizeof buf - n, "\\u%04x", c));
        else buf[n++] = static_cast<char>(c);
    }
    buf[n++] = '"';
    res.write(buf, n);
}

struct Lister {
    HttpResponse* res;
    bool first;
    static void fn(const char* path, size_t size, void* ctx) {
        Lister& l = *static_cast<Lister*>(ctx);
        l.res->print(l.first ? "{\"path\":" : ",{\"path\":");
        l.first = false;
        writeJsonString(*l.res, path);
        char tail[32];
        std::snprintf(tail, sizeof tail, ",\"size\":%lu}", static_cast<unsigned long>(size));
        l.res->print(tail);
    }
};

}  // namespace

const char* HttpFileAdmin::filePath(const HttpRequest& req) {
    const char* p = req.path() + std::strlen("/api/files");
    const size_t n = std::strlen(p);
    if (n < 2 || n > HttpStaticFiles::kMaxPath || p[n - 1] == '/' || !HttpStaticFiles::safePath(p)) return nullptr;
    for (const char* c = p; *c; ++c) {
        if (static_cast<unsigned char>(*c) < 0x20 || *c == 0x7F) return nullptr;
    }
    return p;
}

bool HttpFileAdmin::queryNumber(const HttpRequest& req, const char* name, size_t& out) const {
    char text[16];
    if (!req.param(name, text, sizeof text) || text[0] < '0' || text[0] > '9') return false;
    char* end = nullptr;
    const unsigned long v = std::strtoul(text, &end, 10);
    if (*end != 0 || v > cfg_.maxFileBytes) return false;
    out = v;
    return true;
}

HttpFileAdmin::Done HttpFileAdmin::change(const char* path, bool removeIt, size_t size) {
    auto attempt = [&] { return removeIt ? store_.remove(path) : store_.commit(path, size); };
    if (attempt()) return Done::Ok;
    if (!store_.busy(path)) return Done::Failed;
    if (cfg_.sleep == nullptr) return Done::Busy;
    store_.hold(path, true);   // no new readers; the ones there finish
    Done done = Done::Busy;
    for (uint32_t waited = 0; waited < cfg_.busyWaitMs; waited += 10) {
        cfg_.sleep(10, cfg_.sleepCtx);
        if (attempt()) { done = Done::Ok; break; }
        if (!store_.busy(path)) { done = Done::Failed; break; }
    }
    store_.hold(path, false);
    return done;
}

void HttpFileAdmin::list(const HttpRequest& req, HttpResponse& res, void* self) {
    HttpFileAdmin& a = *static_cast<HttpFileAdmin*>(self);
    if (!a.cfg_.auth->require(req, res, a.cfg_.role, false)) return;
    res.header("Cache-Control", "no-store");
    res.begin("application/json");
    res.print("{\"files\":[");
    Lister l{&res, true};
    const bool ok = a.store_.list(Lister::fn, &l);
    uint64_t total = 0, free = 0;
    a.store_.space(total, free);
    char tail[128];
    std::snprintf(tail, sizeof tail, "],\"ok\":%s,\"total\":%llu,\"free\":%llu,\"piece\":%lu,\"max\":%lu}",
                  ok ? "true" : "false", static_cast<unsigned long long>(total), static_cast<unsigned long long>(free),
                  static_cast<unsigned long>(a.cfg_.pieceBytes), static_cast<unsigned long>(a.cfg_.maxFileBytes));
    res.print(tail);
    res.end();
}

void HttpFileAdmin::put(const HttpRequest& req, HttpResponse& res, void* self) {
    HttpFileAdmin& a = *static_cast<HttpFileAdmin*>(self);
    WebAuth::Identity who;
    if (!a.cfg_.auth->require(req, res, a.cfg_.role, true, &who)) return;
    const char* path = filePath(req);
    if (path == nullptr) return jsonError(res, 400, "not a file path");
    size_t offset = 0;
    if (!a.queryNumber(req, "offset", offset)) return jsonError(res, 400, "offset needed");
    if (offset + req.bodyLength() > a.cfg_.maxFileBytes) return jsonError(res, 413, "file too big");
    if (!a.store_.append(path, offset, req.body(), req.bodyLength())) {
        return jsonError(res, 409, "not at that offset, or the storage is full or failed");
    }
    if (offset == 0 && a.cfg_.audit) a.cfg_.audit(who.user, "upload", path, 0, a.cfg_.auditCtx);
    char body[48];
    std::snprintf(body, sizeof body, "{\"received\":%lu}", static_cast<unsigned long>(offset + req.bodyLength()));
    jsonAnswer(res, 200, body);
}

void HttpFileAdmin::commit(const HttpRequest& req, HttpResponse& res, void* self) {
    HttpFileAdmin& a = *static_cast<HttpFileAdmin*>(self);
    WebAuth::Identity who;
    if (!a.cfg_.auth->require(req, res, a.cfg_.role, true, &who)) return;
    const char* path = filePath(req);
    if (path == nullptr) return jsonError(res, 400, "not a file path");
    size_t size = 0;
    if (!a.queryNumber(req, "size", size)) return jsonError(res, 400, "size needed");
    const Done done = a.change(path, false, size);
    if (done == Done::Busy) return busyAnswer(res);
    if (done == Done::Failed) return jsonError(res, 409, "the upload isn't that size, or isn't there");
    if (a.cfg_.audit) a.cfg_.audit(who.user, "commit", path, size, a.cfg_.auditCtx);
    jsonAnswer(res, 200, "{}");
}

void HttpFileAdmin::remove(const HttpRequest& req, HttpResponse& res, void* self) {
    HttpFileAdmin& a = *static_cast<HttpFileAdmin*>(self);
    WebAuth::Identity who;
    if (!a.cfg_.auth->require(req, res, a.cfg_.role, true, &who)) return;
    const char* path = filePath(req);
    if (path == nullptr) return jsonError(res, 400, "not a file path");
    const Done done = a.change(path, true, 0);
    if (done == Done::Busy) return busyAnswer(res);
    if (done == Done::Failed) return jsonError(res, 404, "no such file");
    if (a.cfg_.audit) a.cfg_.audit(who.user, "delete", path, 0, a.cfg_.auditCtx);
    jsonAnswer(res, 200, "{}");
}
