#include "HttpJson.h"
#include <cstring>

namespace {

struct Scanner {
    const char* p;
    const char* end;

    void ws() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p;
    }
    // A string token, quotes included. false: malformed.
    bool skipString() {
        if (p >= end || *p != '"') return false;
        for (++p; p < end; ++p) {
            if (*p == '\\') {
                if (++p >= end) return false;
            } else if (*p == '"') {
                ++p;
                return true;
            } else if (static_cast<unsigned char>(*p) < 0x20) {
                return false;
            }
        }
        return false;
    }
    // Any value. Depth-limited, so a hostile body can't recurse deeply.
    bool skipValue(int depth = 0) {
        ws();
        if (p >= end || depth > 16) return false;
        if (*p == '"') return skipString();
        if (*p == '{' || *p == '[') {
            const char close = *p == '{' ? '}' : ']';
            const bool object = *p == '{';
            ++p;
            ws();
            if (p < end && *p == close) { ++p; return true; }
            for (;;) {
                if (object) {
                    ws();
                    if (!skipString()) return false;
                    ws();
                    if (p >= end || *p != ':') return false;
                    ++p;
                }
                if (!skipValue(depth + 1)) return false;
                ws();
                if (p >= end) return false;
                if (*p == ',') { ++p; continue; }
                if (*p == close) { ++p; return true; }
                return false;
            }
        }
        // A number, true, false or null: up to the next delimiter.
        const char* start = p;
        while (p < end && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') ++p;
        return p > start;
    }
};

int hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if ((c | 0x20) >= 'a' && (c | 0x20) <= 'f') return (c | 0x20) - 'a' + 10;
    return -1;
}

}  // namespace

bool HttpJson::raw(const char* json, size_t len, const char* key, const char*& value, size_t& valueLen) {
    if (json == nullptr || key == nullptr) return false;
    Scanner s{json, json + len};
    s.ws();
    if (s.p >= s.end || *s.p != '{') return false;
    ++s.p;
    s.ws();
    if (s.p < s.end && *s.p == '}') return false;
    const size_t keyLen = std::strlen(key);
    for (;;) {
        s.ws();
        const char* k = s.p;
        if (!s.skipString()) return false;
        // Keys are compared as written: an escaped key never matches.
        const bool match = static_cast<size_t>(s.p - k) == keyLen + 2 && std::memcmp(k + 1, key, keyLen) == 0;
        s.ws();
        if (s.p >= s.end || *s.p != ':') return false;
        ++s.p;
        s.ws();
        const char* v = s.p;
        if (!s.skipValue()) return false;
        if (match) {
            value = v;
            valueLen = static_cast<size_t>(s.p - v);
            return true;
        }
        s.ws();
        if (s.p >= s.end) return false;
        if (*s.p == ',') { ++s.p; continue; }
        return false;   // '}' (not found) or malformed
    }
}

bool HttpJson::string(const char* json, size_t len, const char* key, char* out, size_t cap) {
    const char* v;
    size_t n;
    if (cap == 0 || !raw(json, len, key, v, n) || n < 2 || v[0] != '"') return false;
    size_t o = 0;
    for (size_t i = 1; i + 1 < n; ++i) {
        uint32_t c = static_cast<unsigned char>(v[i]);
        if (c == '\\') {
            const char e = v[++i];
            switch (e) {
            case '"': case '\\': case '/': c = static_cast<unsigned char>(e); break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case 't': c = '\t'; break;
            case 'u': {
                if (i + 4 >= n) return false;
                c = 0;
                for (int k = 1; k <= 4; ++k) {
                    const int h = hex(v[i + k]);
                    if (h < 0) return false;
                    c = c * 16 + static_cast<uint32_t>(h);
                }
                i += 4;
                // A surrogate pair, as one character.
                if (c >= 0xD800 && c < 0xDC00 && i + 6 < n && v[i + 1] == '\\' && v[i + 2] == 'u') {
                    uint32_t lo = 0;
                    bool ok = true;
                    for (int k = 3; k <= 6 && ok; ++k) {
                        const int h = hex(v[i + k]);
                        ok = h >= 0;
                        lo = lo * 16 + static_cast<uint32_t>(h);
                    }
                    if (ok && lo >= 0xDC00 && lo < 0xE000) {
                        c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                        i += 6;
                    }
                }
                if (c == 0) return false;
                break;
            }
            default: return false;
            }
        }
        // As UTF-8.
        char buf[4];
        size_t m;
        if (c < 0x80) { buf[0] = static_cast<char>(c); m = 1; }
        else if (c < 0x800) { buf[0] = static_cast<char>(0xC0 | (c >> 6)); buf[1] = static_cast<char>(0x80 | (c & 63)); m = 2; }
        else if (c < 0x10000) {
            buf[0] = static_cast<char>(0xE0 | (c >> 12));
            buf[1] = static_cast<char>(0x80 | ((c >> 6) & 63));
            buf[2] = static_cast<char>(0x80 | (c & 63));
            m = 3;
        } else {
            buf[0] = static_cast<char>(0xF0 | (c >> 18));
            buf[1] = static_cast<char>(0x80 | ((c >> 12) & 63));
            buf[2] = static_cast<char>(0x80 | ((c >> 6) & 63));
            buf[3] = static_cast<char>(0x80 | (c & 63));
            m = 4;
        }
        if (o + m >= cap) return false;
        std::memcpy(out + o, buf, m);
        o += m;
    }
    out[o] = 0;
    return true;
}
