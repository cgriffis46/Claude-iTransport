#include "PlcTagWebApi.h"
#include <cmath>
#include <cstdio>
#include <cstring>

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
    PlcTagRegistry& reg = static_cast<PlcTagWebApi*>(self)->registry_;
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
                    writeTag(res, batch[0]);
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
                if (!writeTag(res, batch[i])) return;   // the client went: the connection closes
            }
            if (n < kBatch) break;
            at += n;
        }
    }
    res.print("]}");
    res.end();
}

void PlcTagWebApi::one(const HttpRequest& req, HttpResponse& res, void* self) {
    PlcTagRegistry& reg = static_cast<PlcTagWebApi*>(self)->registry_;
    const char* name = req.path() + std::strlen("/api/tags/");
    PlcTagSnapshot t;
    if (*name == 0 || !reg.snapshot(name, t)) return notFound(res, name);
    res.header("Cache-Control", "no-store");
    res.begin("application/json");
    writeTag(res, t);
    res.end();
}
