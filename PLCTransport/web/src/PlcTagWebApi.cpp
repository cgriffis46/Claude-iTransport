#include "PlcTagWebApi.h"
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include "HttpJson.h"

namespace {

template <typename T>
T load(const uint8_t* p) {
    T v;
    std::memcpy(&v, p, sizeof v);
    return v;
}

// Decimal, without printf: newlib-nano's has no %lld.
size_t formatU64(uint64_t v, bool negative, char* out, size_t cap) {
    char tmp[24];
    size_t n = 0;
    do { tmp[n++] = static_cast<char>('0' + v % 10); v /= 10; } while (v);
    if (negative) tmp[n++] = '-';
    if (n + 1 > cap) { if (cap) out[0] = 0; return 0; }
    for (size_t i = 0; i < n; ++i) out[i] = tmp[n - 1 - i];
    out[n] = 0;
    return n;
}

size_t formatI64(int64_t v, char* out, size_t cap) {
    const uint64_t mag = v < 0 ? static_cast<uint64_t>(-(v + 1)) + 1 : static_cast<uint64_t>(v);
    return formatU64(mag, v < 0, out, cap);
}

size_t formatReal(double v, int digits, char* out, size_t cap) {
    if (std::isnan(v) || std::isinf(v)) return static_cast<size_t>(std::snprintf(out, cap, "null"));
    const int n = std::snprintf(out, cap, "%.*g", digits, v);
    return n < 0 || static_cast<size_t>(n) >= cap ? 0 : static_cast<size_t>(n);
}

// Whether a query string has key (as sent: names are plain ASCII).
bool hasKey(const char* query, const char* key) {
    const size_t n = std::strlen(key);
    for (const char* p = query; *p;) {
        if (std::strncmp(p, key, n) == 0 && (p[n] == '=' || p[n] == '&' || p[n] == 0)) return true;
        p = std::strchr(p, '&');
        if (p == nullptr) break;
        ++p;
    }
    return false;
}

void notFound(HttpResponse& res, const char* name) {
    res.status(404).header("Cache-Control", "no-store");
    res.begin("application/json");
    res.print("{\"name\":");
    PlcTagWebApi::writeString(res, name);
    res.print(",\"error\":\"no such tag\"}");
    res.end();
}

}  // namespace

const char* PlcTagWebApi::typeName(PlcDataType t) {
    switch (t) {
    case PlcDataType::Bool:   return "BOOL";
    case PlcDataType::Sint:   return "SINT";
    case PlcDataType::Int:    return "INT";
    case PlcDataType::Dint:   return "DINT";
    case PlcDataType::Lint:   return "LINT";
    case PlcDataType::Usint:  return "USINT";
    case PlcDataType::Uint:   return "UINT";
    case PlcDataType::Udint:  return "UDINT";
    case PlcDataType::Ulint:  return "ULINT";
    case PlcDataType::Real:   return "REAL";
    case PlcDataType::Lreal:  return "LREAL";
    case PlcDataType::Struct: return "STRUCT";
    }
    return "UNKNOWN";
}

size_t PlcTagWebApi::formatValue(const PlcTagSnapshot& t, char* out, size_t cap) {
    const uint8_t* v = t.value;
    const size_t have = t.sizeBytes < PlcTagSnapshot::kMaxValue ? t.sizeBytes : PlcTagSnapshot::kMaxValue;
    // An elementary type whose size doesn't match is shown as bytes.
    switch (t.dataType) {
    case PlcDataType::Bool:
        if (have >= 1) return static_cast<size_t>(std::snprintf(out, cap, "%s", v[0] ? "true" : "false"));
        break;
    case PlcDataType::Sint:  if (have == 1) return formatI64(load<int8_t>(v), out, cap); break;
    case PlcDataType::Int:   if (have == 2) return formatI64(load<int16_t>(v), out, cap); break;
    case PlcDataType::Dint:  if (have == 4) return formatI64(load<int32_t>(v), out, cap); break;
    case PlcDataType::Lint:  if (have == 8) return formatI64(load<int64_t>(v), out, cap); break;
    case PlcDataType::Usint: if (have == 1) return formatU64(load<uint8_t>(v), false, out, cap); break;
    case PlcDataType::Uint:  if (have == 2) return formatU64(load<uint16_t>(v), false, out, cap); break;
    case PlcDataType::Udint: if (have == 4) return formatU64(load<uint32_t>(v), false, out, cap); break;
    case PlcDataType::Ulint: if (have == 8) return formatU64(load<uint64_t>(v), false, out, cap); break;
    case PlcDataType::Real:  if (have == 4) return formatReal(load<float>(v), 9, out, cap); break;
    case PlcDataType::Lreal: if (have == 8) return formatReal(load<double>(v), 17, out, cap); break;
    case PlcDataType::Struct: break;
    }
    // Bytes, as a hex string.
    static const char kHex[] = "0123456789abcdef";
    if (cap < 2 * have + 3) { if (cap) out[0] = 0; return 0; }
    size_t n = 0;
    out[n++] = '"';
    for (size_t i = 0; i < have; ++i) {
        out[n++] = kHex[v[i] >> 4];
        out[n++] = kHex[v[i] & 15];
    }
    out[n++] = '"';
    out[n] = 0;
    return n;
}

bool PlcTagWebApi::writeString(HttpResponse& res, const char* s) {
    char buf[96];
    size_t n = 0;
    buf[n++] = '"';
    for (const char* p = s; *p; ++p) {
        if (n > sizeof buf - 8) {
            if (!res.write(buf, n)) return false;
            n = 0;
        }
        const unsigned char c = static_cast<unsigned char>(*p);
        if (c == '"' || c == '\\') {
            buf[n++] = '\\';
            buf[n++] = static_cast<char>(c);
        } else if (c < 0x20) {
            n += static_cast<size_t>(std::snprintf(buf + n, sizeof buf - n, "\\u%04x", c));
        } else {
            buf[n++] = static_cast<char>(c);
        }
    }
    buf[n++] = '"';
    return res.write(buf, n);
}

bool PlcTagWebApi::writeTag(HttpResponse& res, const PlcTagSnapshot& t) {
    char value[2 * PlcTagSnapshot::kMaxValue + 4];
    formatValue(t, value, sizeof value);
    bool ok = res.print("{\"name\":") && writeString(res, t.name) && res.print(",\"type\":\"") &&
              res.print(typeName(t.dataType)) && res.print("\",\"value\":") && res.print(value);
    if (t.dataType == PlcDataType::Struct || std::strchr(value, '"') == value) {
        char size[16];
        formatU64(t.sizeBytes, false, size, sizeof size);
        ok = ok && res.print(",\"size\":") && res.print(size);
        if (t.sizeBytes > PlcTagSnapshot::kMaxValue) ok = ok && res.print(",\"truncated\":true");
    }
    return ok && res.print(t.writable ? ",\"writable\":true}" : ",\"writable\":false}");
}

void PlcTagWebApi::list(const HttpRequest& req, HttpResponse& res, void* self) {
    PlcTagWebApi& api = *static_cast<PlcTagWebApi*>(self);
    if (!api.mayRead(req, res)) return;
    PlcTagRegistry& reg = api.registry_;
    char names[kMaxNames];
    const bool some = req.param("names", names, sizeof names);
    if (!some && hasKey(req.query(), "names")) {
        // There, but too long for the buffer: say so, rather than answer
        // for a different list.
        res.status(414).send("application/json", "{\"error\":\"names list too long\"}");
        return;
    }
    res.header("Cache-Control", "no-store");
    res.begin("application/json");
    res.print("{\"tags\":[");
    PlcTagSnapshot batch[kBatch];
    bool first = true;
    if (some) {
        // In the order asked, one at a time.
        for (char* name = names; name && *name;) {
            char* comma = std::strchr(name, ',');
            if (comma) *comma = 0;
            if (*name) {
                if (!first) res.print(",");
                first = false;
                if (reg.snapshot(name, batch[0])) {
                    api.writeTagJson(res, batch[0]);
                } else {
                    res.print("{\"name\":");
                    writeString(res, name);
                    res.print(",\"error\":\"no such tag\"}");
                }
            }
            name = comma ? comma + 1 : nullptr;
        }
    } else {
        for (size_t at = 0;;) {
            const size_t n = reg.snapshot(at, batch, kBatch);   // the lock is held only in here
            for (size_t i = 0; i < n; ++i) {
                if (!first) res.print(",");
                first = false;
                if (!api.writeTagJson(res, batch[i])) return;   // the client went: the connection closes
            }
            if (n < kBatch) break;
            at += n;
        }
    }
    res.print("]}");
    res.end();
}

void PlcTagWebApi::one(const HttpRequest& req, HttpResponse& res, void* self) {
    PlcTagWebApi& api = *static_cast<PlcTagWebApi*>(self);
    if (!api.mayRead(req, res)) return;
    PlcTagRegistry& reg = api.registry_;
    const char* name = req.path() + std::strlen("/api/tags/");
    PlcTagSnapshot t;
    if (*name == 0 || !reg.snapshot(name, t)) return notFound(res, name);
    res.header("Cache-Control", "no-store");
    res.begin("application/json");
    api.writeTagJson(res, t);
    res.end();
}

// ---- with logins: reading by role, and writing ----

bool PlcTagWebApi::mayRead(const HttpRequest& req, HttpResponse& res) {
    if (cfg_.auth == nullptr || cfg_.readRole == WebRole::None) return true;
    return cfg_.auth->require(req, res, cfg_.readRole, false);
}

const PlcWebWritable* PlcTagWebApi::writableEntry(const char* name) const {
    for (size_t i = 0; i < cfg_.writableCount; ++i) {
        if (std::strcmp(cfg_.writable[i].name, name) == 0) return &cfg_.writable[i];
    }
    return nullptr;
}

bool PlcTagWebApi::writeTagJson(HttpResponse& res, const PlcTagSnapshot& t) const {
    const PlcWebWritable* w = cfg_.auth ? writableEntry(t.name) : nullptr;
    if (w == nullptr || !t.writable || t.dataType == PlcDataType::Struct) return writeTag(res, t);
    // As writeTag(), with the web's permission and limits.
    char value[2 * PlcTagSnapshot::kMaxValue + 4];
    formatValue(t, value, sizeof value);
    bool ok = res.print("{\"name\":") && writeString(res, t.name) && res.print(",\"type\":\"") &&
              res.print(typeName(t.dataType)) && res.print("\",\"value\":") && res.print(value) &&
              res.print(",\"writable\":true,\"webWritable\":true");
    if (t.dataType != PlcDataType::Bool) {
        char limits[64];
        std::snprintf(limits, sizeof limits, ",\"min\":%.17g,\"max\":%.17g", w->min, w->max);
        ok = ok && res.print(limits);
    }
    return ok && res.print("}");
}

bool PlcTagWebApi::parseValue(PlcDataType t, const char* text, size_t len, uint8_t out[8], double& asDouble) {
    char buf[40];
    if (len == 0 || len >= sizeof buf) return false;
    std::memcpy(buf, text, len);
    buf[len] = 0;
    if (t == PlcDataType::Bool) {
        const bool v = std::strcmp(buf, "true") == 0 || std::strcmp(buf, "1") == 0;
        if (!v && std::strcmp(buf, "false") != 0 && std::strcmp(buf, "0") != 0) return false;
        out[0] = v ? 1 : 0;
        asDouble = v ? 1 : 0;
        return true;
    }
    char* end = nullptr;
    errno = 0;
    if (t == PlcDataType::Real || t == PlcDataType::Lreal) {
        const double d = std::strtod(buf, &end);
        if (end == buf || *end != 0 || errno == ERANGE || !std::isfinite(d)) return false;
        if (t == PlcDataType::Real) {
            if (std::fabs(d) > std::numeric_limits<float>::max()) return false;
            const float f = static_cast<float>(d);
            std::memcpy(out, &f, sizeof f);
        } else {
            std::memcpy(out, &d, sizeof d);
        }
        asDouble = d;
        return true;
    }
    // Integers: exactly, all 64 bits, no fraction, no exponent.
    const bool isUnsigned = t == PlcDataType::Usint || t == PlcDataType::Uint || t == PlcDataType::Udint ||
                            t == PlcDataType::Ulint;
    if (isUnsigned) {
        if (buf[0] == '-') return false;
        const unsigned long long v = std::strtoull(buf, &end, 10);
        if (end == buf || *end != 0 || errno == ERANGE) return false;
        const unsigned long long max = t == PlcDataType::Usint ? 0xFFull : t == PlcDataType::Uint ? 0xFFFFull
                                     : t == PlcDataType::Udint ? 0xFFFFFFFFull : ~0ull;
        if (v > max) return false;
        const uint64_t u = v;
        const size_t n = t == PlcDataType::Usint ? 1 : t == PlcDataType::Uint ? 2 : t == PlcDataType::Udint ? 4 : 8;
        std::memcpy(out, &u, n);   // little-endian, as on the MCU
        asDouble = static_cast<double>(v);
        return true;
    }
    if (t == PlcDataType::Sint || t == PlcDataType::Int || t == PlcDataType::Dint || t == PlcDataType::Lint) {
        const long long v = std::strtoll(buf, &end, 10);
        if (end == buf || *end != 0 || errno == ERANGE) return false;
        const long long lo = t == PlcDataType::Sint ? -128 : t == PlcDataType::Int ? -32768
                           : t == PlcDataType::Dint ? -2147483648LL : std::numeric_limits<long long>::min();
        const long long hi = t == PlcDataType::Sint ? 127 : t == PlcDataType::Int ? 32767
                           : t == PlcDataType::Dint ? 2147483647LL : std::numeric_limits<long long>::max();
        if (v < lo || v > hi) return false;
        const int64_t s = v;
        const size_t n = t == PlcDataType::Sint ? 1 : t == PlcDataType::Int ? 2 : t == PlcDataType::Dint ? 4 : 8;
        std::memcpy(out, &s, n);
        asDouble = static_cast<double>(v);
        return true;
    }
    return false;   // STRUCT: not from the web
}

namespace {
void writeError(HttpResponse& res, uint16_t status, const char* message) {
    char body[96];
    std::snprintf(body, sizeof body, "{\"error\":\"%s\"}", message);
    res.status(status).header("Cache-Control", "no-store");
    res.send("application/json", body);
}
}  // namespace

void PlcTagWebApi::write(const HttpRequest& req, HttpResponse& res, void* self) {
    PlcTagWebApi& api = *static_cast<PlcTagWebApi*>(self);
    WebAuth::Identity who;
    if (api.cfg_.auth == nullptr || !api.cfg_.auth->require(req, res, api.cfg_.writeRole, true, &who)) return;

    const char* name = req.path() + std::strlen("/api/tags/");
    PlcTagSnapshot before;
    if (*name == 0 || !api.registry_.snapshot(name, before)) return writeError(res, 404, "no such tag");
    const PlcWebWritable* w = api.writableEntry(name);
    if (w == nullptr) return writeError(res, 403, "not writable from the web");
    if (!before.writable || before.dataType == PlcDataType::Struct) return writeError(res, 403, "tag is read-only");

    // The value: {"value":...} or value=...
    const char* text = nullptr;
    size_t len = 0;
    char form[40];
    const char* type = req.header("Content-Type");
    if (type && std::strncmp(type, "application/json", 16) == 0) {
        if (!HttpJson::raw(reinterpret_cast<const char*>(req.body()), req.bodyLength(), "value", text, len)) {
            return writeError(res, 400, "a value is needed");
        }
    } else if (req.param("value", form, sizeof form)) {
        text = form;
        len = std::strlen(form);
    } else {
        return writeError(res, 400, "a value is needed");
    }

    uint8_t bytes[8];
    double asDouble = 0;
    if (!parseValue(before.dataType, text, len, bytes, asDouble)) {
        return writeError(res, 422, "not a value of the tag's type, or out of its range");
    }
    if (before.dataType != PlcDataType::Bool && (asDouble < w->min || asDouble > w->max)) {
        return writeError(res, 422, "outside the allowed range");
    }
    if (!api.registry_.writeTag(name, bytes, before.sizeBytes)) return writeError(res, 409, "the write failed");

    PlcTagSnapshot after;
    api.registry_.snapshot(name, after);
    if (api.cfg_.audit) api.cfg_.audit(who.user, before, after, api.cfg_.auditCtx);
    res.header("Cache-Control", "no-store");
    res.begin("application/json");
    api.writeTagJson(res, after);
    res.end();
}
