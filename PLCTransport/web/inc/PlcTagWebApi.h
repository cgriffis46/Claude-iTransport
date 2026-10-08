#pragma once
#include <cstddef>
#include <cstdint>
#include "HttpConnection.h"
#include "PlcTagRegistry.h"
#include "WebAuth.h"

// A tag the web may write, and the range it may be set to (ignored for
// BOOL). Anything not listed can't be written from the web, whatever the
// registry allows.
struct PlcWebWritable {
    const char* name;
    double      min;
    double      max;
};

// The PLC tag database as JSON, read-only, for a web page or a script:
//
//   GET /api/tags              every tag
//   GET /api/tags?names=a,b,c  those, in that order
//   GET /api/tags/<name>       one (the name percent-encoded if it needs it)
//
//   {"tags":[{"name":"Motor1.Speed","type":"REAL","value":1500.5,"writable":true}, ...]}
//
// A value is a JSON number for the integer and real types (NaN and the
// infinities as null; 64-bit integers beyond 2^53 lose precision in
// JavaScript), true/false for BOOL, and a hex string for a STRUCT
// (its first 64 bytes, with "size", and "truncated":true if longer). An
// unknown name in a list gets {"name":"x","error":"no such tag"}; on its
// own, a 404 with that body.
//
// Values are copied from the registry a few tags at a time, each batch
// under one hold of the registry's lock, and written out after it is
// released: a slow client never holds up the control program or the CIP
// server.
//
// Writes, only with a WebAuth (Config::auth) and an allow-list:
//
//   POST /api/tags/<name>   {"value":42} (or a form: value=42), with the
//                           session cookie and an X-CSRF-Token header
//
//   200 and the tag as it is now; 401 no login; 403 the role is too low,
//   no CSRF token, or the tag isn't on the list or isn't writable; 404 no
//   such tag; 400 not a value; 422 out of the tag's type's range or the
//   list's limits.
//
// Every write goes to Config::audit (who, the tag before and after).
// When there is a WebAuth, listed tags show "webWritable":true and their
// limits in the GETs, so a page knows what to offer.
//
//     static PlcTagWebApi api(registry);      // read-only
//     api.attach(web);                        // before any "/*" route
//
// Formatting a REAL or LREAL uses snprintf("%g"): with newlib-nano, link
// with -u _printf_float.
class PlcTagWebApi {
public:
    static constexpr size_t kBatch = 4;          // tags copied per hold of the lock
    static constexpr size_t kMaxNames = 256;     // the names= list, decoded

    typedef void (*AuditFn)(const char* user, const PlcTagSnapshot& before, const PlcTagSnapshot& after, void* ctx);

    struct Config {
        WebAuth*              auth = nullptr;          // none: read-only, and reads need no login
        WebRole               readRole = WebRole::None;     // with auth: who may read (None: anyone)
        WebRole               writeRole = WebRole::Operator;
        const PlcWebWritable* writable = nullptr;
        size_t                writableCount = 0;
        AuditFn               audit = nullptr;
        void*                 auditCtx = nullptr;
    };

    explicit PlcTagWebApi(PlcTagRegistry& registry) : PlcTagWebApi(registry, Config()) {}
    PlcTagWebApi(PlcTagRegistry& registry, const Config& cfg) : registry_(registry), cfg_(cfg) {}

    // Adds the routes to an xHttpServer or HttpRoutes: the GETs, and the
    // POST when there are logins and an allow-list.
    template <typename Server>
    bool attach(Server& server) {
        bool ok = server.on(HttpMethod::Get, "/api/tags", list, this) &&
                  server.on(HttpMethod::Get, "/api/tags/*", one, this);
        if (cfg_.auth && cfg_.writableCount) ok = ok && server.on(HttpMethod::Post, "/api/tags/*", write, this);
        return ok;
    }

    static void list(const HttpRequest& req, HttpResponse& res, void* self);
    static void one(const HttpRequest& req, HttpResponse& res, void* self);
    static void write(const HttpRequest& req, HttpResponse& res, void* self);

    // text as a value of type t, into out (the tag's bytes) and asDouble.
    // false: not a value of that type, or outside its range.
    static bool parseValue(PlcDataType t, const char* text, size_t len, uint8_t out[8], double& asDouble);

    // JSON pieces, for pages of one's own.
    static const char* typeName(PlcDataType t);
    static bool writeTag(HttpResponse& res, const PlcTagSnapshot& t);
    static bool writeString(HttpResponse& res, const char* s);
    // The value as JSON text into out. Returns its length.
    static size_t formatValue(const PlcTagSnapshot& t, char* out, size_t cap);

private:
    const PlcWebWritable* writableEntry(const char* name) const;
    bool writeTagJson(HttpResponse& res, const PlcTagSnapshot& t) const;   // with webWritable and limits
    bool mayRead(const HttpRequest& req, HttpResponse& res);

    PlcTagRegistry& registry_;
    Config          cfg_;
};
