#include "HttpClientProtocol.h"
#include <cstdio>
#include <cstring>

namespace {

bool startsWithNoCase(const char* s, const char* prefix) {
    for (; *prefix; ++s, ++prefix) {
        if (*s == 0 || (*s | 0x20) != *prefix) return false;
    }
    return true;
}

bool equalsNoCase(const char* a, const char* b) {
    for (; *a && *b; ++a, ++b) {
        if ((*a | 0x20) != (*b | 0x20)) return false;
    }
    return *a == *b;
}

// Whether a comma-separated header value (Connection) has this token.
bool hasToken(const char* list, const char* token) {
    const size_t n = std::strlen(token);
    for (const char* p = list; *p;) {
        while (*p == ' ' || *p == '\t' || *p == ',') ++p;
        const char* e = p;
        while (*e && *e != ',') ++e;
        const char* t = e;
        while (t > p && (t[-1] == ' ' || t[-1] == '\t')) --t;
        if (static_cast<size_t>(t - p) == n) {
            bool same = true;
            for (size_t i = 0; i < n && same; ++i) same = ((p[i] | 0x20) == (token[i] | 0x20));
            if (same) return true;
        }
        p = e;
    }
    return false;
}

const char* const kMethodNames[] = {"GET", "HEAD", "POST", "PUT", "DELETE", "OPTIONS", "PATCH"};

}  // namespace

// ---- HttpUrl ----

bool HttpUrl::parse(const char* url, HttpUrl& out) {
    out = HttpUrl();
    if (url == nullptr) return false;
    for (const char* p = url; *p; ++p) {
        if (static_cast<uint8_t>(*p) <= 0x20 || *p == 0x7F) return false;
    }
    const char* p;
    if (startsWithNoCase(url, "http://")) {
        p = url + 7;
    } else if (startsWithNoCase(url, "https://")) {
        p = url + 8;
        out.https = true;
        out.port = 443;
    } else {
        return false;
    }
    if (std::strchr(p, '@')) {   // user:pass@host: not supported, and easy to misread
        const char* at = std::strchr(p, '@');
        const char* slash = std::strpbrk(p, "/?#");
        if (slash == nullptr || at < slash) return false;
    }
    out.host = p;
    while (*p && *p != ':' && *p != '/' && *p != '?' && *p != '#') ++p;
    out.hostLen = static_cast<size_t>(p - out.host);
    if (out.hostLen == 0) return false;
    if (*p == ':') {
        ++p;
        uint32_t port = 0;
        const char* digits = p;
        while (*p >= '0' && *p <= '9') {
            port = port * 10 + static_cast<uint32_t>(*p - '0');
            if (port > 65535) return false;
            ++p;
        }
        if (p == digits || port == 0) return false;
        out.port = static_cast<uint16_t>(port);
    }
    if (*p != 0 && *p != '/' && *p != '?' && *p != '#') return false;
    if (*p == 0 || *p == '#') {
        out.path = "/";
        out.pathLen = 1;
        return true;
    }
    out.path = p;   // "/x?y", or "?y" (sent as "/?y" by the writer)
    const char* frag = std::strchr(p, '#');   // a fragment is never sent
    out.pathLen = frag ? static_cast<size_t>(frag - p) : std::strlen(p);
    return true;
}

// ---- the request head ----

bool httpWriteRequestHead(HttpOutput& out, HttpMethod method, const char* hostHeader, const char* path,
                          size_t pathLen, const char* contentType, int32_t contentLength,
                          const char* extraHeaders, const char* userAgent, bool keepAlive) {
    if (method == HttpMethod::Other) return false;
    const char* name = kMethodNames[static_cast<uint8_t>(method)];
    bool ok = out.write(name, std::strlen(name)) && out.write(" ", 1);
    if (pathLen && path[0] == '?') ok = ok && out.write("/", 1);
    ok = ok && out.write(path, pathLen) && out.write(" HTTP/1.1\r\nHost: ", 17) &&
         out.write(hostHeader, std::strlen(hostHeader)) && out.write("\r\n", 2);
    if (userAgent && *userAgent) {
        ok = ok && out.write("User-Agent: ", 12) && out.write(userAgent, std::strlen(userAgent)) &&
             out.write("\r\n", 2);
    }
    if (!keepAlive) ok = ok && out.write("Connection: close\r\n", 19);
    const bool hasBody = contentLength >= 0 || method == HttpMethod::Post || method == HttpMethod::Put ||
                         method == HttpMethod::Patch;
    if (hasBody) {
        if (contentType && *contentType) {
            ok = ok && out.write("Content-Type: ", 14) && out.write(contentType, std::strlen(contentType)) &&
                 out.write("\r\n", 2);
        }
        char line[32];
        const int n = std::snprintf(line, sizeof line, "Content-Length: %ld\r\n",
                                    static_cast<long>(contentLength < 0 ? 0 : contentLength));
        ok = ok && out.write(line, static_cast<size_t>(n));
    }
    if (extraHeaders && *extraHeaders) ok = ok && out.write(extraHeaders, std::strlen(extraHeaders));
    return ok && out.write("\r\n", 2);
}

// ---- HttpResponseReader ----

void HttpResponseReader::begin(uint8_t* body, size_t cap, BodyFn onBody, HeaderFn onHeader, void* ctx) {
    body_ = body;
    cap_ = body ? cap : 0;
    onBody_ = onBody;
    onHeader_ = onHeader;
    ctx_ = ctx;
    st_ = St::Status;
    status_ = 0;
    http11_ = true;
    closeHeader_ = keepAliveHeader_ = keepAlive_ = truncated_ = false;
    length_ = 0;
    bodyLen_ = 0;
    nameLen_ = valueLen_ = 0;
    inValue_ = false;
    if (body_ && cap_) body_[0] = 0;
}

void HttpResponseReader::headerDone() {
    name_[nameLen_] = 0;
    value_[valueLen_] = 0;
    if (equalsNoCase(name_, "Connection")) {
        if (hasToken(value_, "close")) closeHeader_ = true;
        if (hasToken(value_, "keep-alive")) keepAliveHeader_ = true;
    }
    if (onHeader_) onHeader_(name_, value_, ctx_);
    nameLen_ = valueLen_ = 0;
    inValue_ = false;
}

bool HttpResponseReader::put(const HttpToken& t) {
    if (st_ == St::Done || st_ == St::Failed || st_ == St::Aborted) return true;   // nothing more wanted
    if (t.type == HttpTokenType::Error) { st_ = St::Failed; return true; }

    switch (st_) {
    case St::Status:
        // A Version token, then Status; the Reason is ignored.
        if (t.type == HttpTokenType::Version) {
            if (t.len != 8 || std::memcmp(t.text, "HTTP/1.", 7) != 0 || t.text[7] < '0' || t.text[7] > '9') {
                st_ = St::Failed;
            } else {
                http11_ = t.text[7] != '0';
            }
        } else if (t.type == HttpTokenType::Status) {
            status_ = static_cast<uint16_t>(t.num);
            closeHeader_ = keepAliveHeader_ = false;
            nameLen_ = valueLen_ = 0;
            inValue_ = false;
            st_ = St::Headers;
        }
        return true;

    case St::Headers:
        if (t.type == HttpTokenType::Reason) return true;
        if (t.type == HttpTokenType::HeaderName) {
            if (inValue_) headerDone();   // can't happen: the lexer always sends a value
            for (uint16_t i = 0; i < t.len && nameLen_ < kName - 1; ++i) name_[nameLen_++] = t.text[i];
            return true;
        }
        if (t.type == HttpTokenType::HeaderValue) {
            inValue_ = true;
            for (uint16_t i = 0; i < t.len && valueLen_ < kValue - 1; ++i) value_[valueLen_++] = t.text[i];
            if (!t.more) headerDone();
            return true;
        }
        if (t.type == HttpTokenType::HeadersEnd) {
            if (status_ < 200) {   // 100 Continue and the like: the real response follows
                st_ = St::Status;
                return true;
            }
            length_ = t.num;
            keepAlive_ = (http11_ ? !closeHeader_ : keepAliveHeader_) && t.num != HttpToken::kToClose;
            st_ = St::Body;
            return true;
        }
        st_ = St::Failed;
        return true;

    case St::Body:
        if (t.type == HttpTokenType::Body) {
            bodyLen_ += t.len;
            if (onBody_) {
                if (!onBody_(reinterpret_cast<const uint8_t*>(t.text), t.len, ctx_)) st_ = St::Aborted;
                return true;
            }
            const size_t had = bodyLen_ - t.len;
            if (had < cap_) {
                const size_t n = t.len < cap_ - had ? t.len : cap_ - had;
                std::memcpy(body_ + had, t.text, n);
                if (n < t.len) truncated_ = true;
            } else if (t.len) {
                truncated_ = true;
            }
            return true;
        }
        if (t.type == HttpTokenType::MessageEnd) {
            if (body_ && bodyLen_ < cap_) body_[bodyLen_] = 0;   // a text body reads as a string
            st_ = St::Done;
            return true;
        }
        st_ = St::Failed;
        return true;

    default:
        return true;
    }
}
