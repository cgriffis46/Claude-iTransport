#include "HttpRequest.h"
#include <cstring>

namespace {

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool equalsNoCase(const char* a, const char* b) {
    for (; *a && *b; ++a, ++b) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = static_cast<char>(x + 32);
        if (y >= 'A' && y <= 'Z') y = static_cast<char>(y + 32);
        if (x != y) return false;
    }
    return *a == *b;
}

bool startsWithNoCase(const char* s, const char* prefix) {
    for (; *prefix; ++s, ++prefix) {
        char x = *s, y = *prefix;
        if (x >= 'A' && x <= 'Z') x = static_cast<char>(x + 32);
        if (x != y) return false;
    }
    return true;
}

}  // namespace

int32_t HttpRequest::urlDecode(const char* in, size_t len, char* out, size_t cap, bool plusIsSpace) {
    if (cap == 0) return -1;
    size_t n = 0;
    for (size_t i = 0; i < len; ++i) {
        char c = in[i];
        if (c == '%' && i + 2 < len && hexValue(in[i + 1]) >= 0 && hexValue(in[i + 2]) >= 0) {
            c = static_cast<char>(hexValue(in[i + 1]) * 16 + hexValue(in[i + 2]));
            i += 2;
            if (c == 0) { out[0] = 0; return -1; }
        } else if (c == '+' && plusIsSpace) {
            c = ' ';
        }
        // A '%' not followed by two hex digits stays as it is.
        if (n + 1 >= cap) { out[0] = 0; return -1; }
        out[n++] = c;
    }
    out[n] = 0;
    return static_cast<int32_t>(n);
}

const char* HttpRequest::header(const char* name) const {
    const char* p = headers_;
    for (size_t i = 0; i < headerCount_; ++i) {
        const char* value = p + std::strlen(p) + 1;
        if (equalsNoCase(p, name)) return value;
        p = value + std::strlen(value) + 1;
    }
    return nullptr;
}

bool HttpRequest::headerAt(size_t i, const char*& name, const char*& value) const {
    if (i >= headerCount_) return false;
    const char* p = headers_;
    for (size_t k = 0;; ++k) {
        const char* v = p + std::strlen(p) + 1;
        if (k == i) { name = p; value = v; return true; }
        p = v + std::strlen(v) + 1;
    }
}

bool HttpRequest::findParam(const char* s, size_t len, const char* name, char* out, size_t cap, bool& found) {
    const size_t nameLen = std::strlen(name);
    size_t at = 0;
    while (at < len) {
        size_t end = at;
        while (end < len && s[end] != '&') ++end;
        size_t eq = at;
        while (eq < end && s[eq] != '=') ++eq;
        // Compare the decoded key with name.
        char key[48];
        const int32_t k = urlDecode(s + at, eq - at, key, sizeof key, true);
        if (k >= 0 && static_cast<size_t>(k) == nameLen && std::memcmp(key, name, nameLen) == 0) {
            found = true;
            const size_t vAt = eq < end ? eq + 1 : end;
            return urlDecode(s + vAt, end - vAt, out, cap, true) >= 0;
        }
        at = end + 1;
    }
    return false;
}

bool HttpRequest::param(const char* name, char* out, size_t cap) const {
    if (out == nullptr || cap == 0 || name == nullptr) return false;
    out[0] = 0;
    bool found = false;
    if (findParam(query_, std::strlen(query_), name, out, cap, found)) return true;
    if (found) return false;   // there, but too long for out
    const char* type = header("Content-Type");
    if (type && body_ && startsWithNoCase(type, "application/x-www-form-urlencoded")) {
        return findParam(reinterpret_cast<const char*>(body_), bodyLen_, name, out, cap, found);
    }
    return false;
}
