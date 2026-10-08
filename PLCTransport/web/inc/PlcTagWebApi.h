#pragma once
#include <cstddef>
#include <cstdint>
#include "HttpConnection.h"
#include "PlcTagRegistry.h"

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
// server. Writes are deliberately not here (see the README: they need
// authentication first).
//
//     static PlcTagWebApi api(registry);
//     api.attach(web);                        // before any "/*" route
//
// Formatting a REAL or LREAL uses snprintf("%g"): with newlib-nano, link
// with -u _printf_float.
class PlcTagWebApi {
public:
    static constexpr size_t kBatch = 4;          // tags copied per hold of the lock
    static constexpr size_t kMaxNames = 256;     // the names= list, decoded

    explicit PlcTagWebApi(PlcTagRegistry& registry) : registry_(registry) {}

    // Adds the routes to an xHttpServer or HttpRoutes.
    template <typename Server>
    bool attach(Server& server) {
        return server.on(HttpMethod::Get, "/api/tags", list, this) &&
               server.on(HttpMethod::Get, "/api/tags/*", one, this);
    }

    static void list(const HttpRequest& req, HttpResponse& res, void* self);
    static void one(const HttpRequest& req, HttpResponse& res, void* self);

    // JSON pieces, for pages of one's own.
    static const char* typeName(PlcDataType t);
    static bool writeTag(HttpResponse& res, const PlcTagSnapshot& t);
    static bool writeString(HttpResponse& res, const char* s);
    // The value as JSON text into out. Returns its length.
    static size_t formatValue(const PlcTagSnapshot& t, char* out, size_t cap);

private:
    PlcTagRegistry& registry_;
};
