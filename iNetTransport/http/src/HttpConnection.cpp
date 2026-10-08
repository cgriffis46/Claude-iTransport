#include "HttpConnection.h"
#include <cstring>

namespace {

const char* const kMethodNames[] = {"GET", "HEAD", "POST", "PUT", "DELETE", "OPTIONS", "PATCH"};

HttpMethod methodFrom(const char* s, size_t n) {
    for (uint8_t i = 0; i < sizeof kMethodNames / sizeof kMethodNames[0]; ++i) {
        if (std::strlen(kMethodNames[i]) == n && std::memcmp(kMethodNames[i], s, n) == 0) {
            return static_cast<HttpMethod>(i);
        }
    }
    return HttpMethod::Other;
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

}  // namespace

// ---- HttpRoutes ----

bool HttpRoutes::add(HttpMethod method, const char* path, HttpHandler fn, void* ctx) {
    if (n_ == kMaxRoutes || path == nullptr || fn == nullptr || method == HttpMethod::Other) return false;
    routes_[n_++] = Route{method, path, fn, ctx};
    return true;
}

bool HttpRoutes::pathMatches(const char* pattern, const char* path) {
    const size_t n = std::strlen(pattern);
    if (n && pattern[n - 1] == '*') return std::strncmp(pattern, path, n - 1) == 0;
    return std::strcmp(pattern, path) == 0;
}

uint16_t HttpRoutes::find(HttpMethod method, const char* path, HttpHandler& fn, void*& ctx, char* allow,
                          size_t allowCap) const {
    uint8_t methods = 0;   // a bit per HttpMethod the path takes
    for (uint8_t i = 0; i < n_; ++i) {
        const Route& r = routes_[i];
        if (!pathMatches(r.path, path)) continue;
        if (r.method == method || (method == HttpMethod::Head && r.method == HttpMethod::Get)) {
            fn = r.fn;
            ctx = r.ctx;
            return 0;
        }
        methods |= static_cast<uint8_t>(1u << static_cast<uint8_t>(r.method));
        if (r.method == HttpMethod::Get) methods |= 1u << static_cast<uint8_t>(HttpMethod::Head);
    }
    // For the Allow header: "GET, HEAD, POST".
    size_t at = 0;
    if (allowCap) allow[0] = 0;
    for (uint8_t m = 0; m < sizeof kMethodNames / sizeof kMethodNames[0]; ++m) {
        if (!(methods & (1u << m))) continue;
        const size_t n = std::strlen(kMethodNames[m]);
        if (at + n + 3 > allowCap) break;
        if (at) { allow[at++] = ','; allow[at++] = ' '; }
        std::memcpy(allow + at, kMethodNames[m], n + 1);
        at += n;
    }
    return methods ? 405 : 404;
}

// ---- HttpConnection ----

HttpConnection::HttpConnection(const HttpRoutes& routes, uint8_t* arena, size_t arenaSize, HttpOutput& out,
                               uint16_t maxRequests)
    : routes_(routes), arena_(arena), size_(arenaSize), res_(out), out_(out), maxRequests_(maxRequests) {}

void HttpConnection::reset() {
    st_ = St::Idle;
    done_ = false;
    requests_ = errors_ = 0;
    used_ = mark_ = 0;
}

bool HttpConnection::append(const char* s, size_t n, uint16_t codeIfFull) {
    if (size_ - used_ < n + 1) {   // room for the NUL too
        fail(codeIfFull);
        return false;
    }
    std::memcpy(arena_ + used_, s, n);
    used_ += n;
    return true;
}

bool HttpConnection::endString(uint16_t codeIfFull) {
    if (used_ >= size_) { fail(codeIfFull); return false; }
    arena_[used_++] = 0;
    return true;
}

void HttpConnection::beginRequest() {
    used_ = 0;
    req_ = HttpRequest();
    req_.secure_ = secure_;
    keepAlive_ = true;
}

void HttpConnection::fail(uint16_t code) {
    if (done_) return;
    st_ = St::Skip;
    answer(code);
    done_ = true;
}

// A response made here: errors, and requests no route takes. Only a
// 404 or 405 keeps the connection: the request was read whole.
void HttpConnection::answer(uint16_t code, const char* allow) {
    const bool keep = (code == 404 || code == 405) && keepAlive_ && requests_ + 1 < maxRequests_;
    res_.reset(req_.minor_ >= 1, keep, req_.method_ == HttpMethod::Head);
    res_.status(code);
    if (allow && *allow) res_.header("Allow", allow);
    res_.sendStatus(code);
    ++requests_;
    ++errors_;
    if (res_.failed() || !res_.keepAlive()) done_ = true;
}

void HttpConnection::timeout() {
    if (done_) return;
    if (st_ != St::Idle) fail(408);
    done_ = true;
}

void HttpConnection::onToken(const HttpToken& t) {
    if (done_) return;
    if (t.type == HttpTokenType::Error) return fail(static_cast<uint16_t>(t.num));

    switch (st_) {
    case St::Idle:
        if (t.type != HttpTokenType::Method) return fail(400);
        beginRequest();
        req_.method_ = methodFrom(t.text, t.len);
        if (req_.method_ == HttpMethod::Other) return fail(501);
        std::memcpy(req_.methodName_, t.text, t.len);   // a known method: under 8 characters
        req_.methodName_[t.len] = 0;
        mark_ = used_;
        st_ = St::Target;
        return;

    case St::Target:
        if (t.type != HttpTokenType::Target) return fail(400);
        if (!append(t.text, t.len, 414)) return;
        if (t.more) return;
        if (!endString(414) || !targetDone()) return;
        st_ = St::Version;
        return;

    case St::Version:
        // "HTTP/1.0" or "HTTP/1.1"; a later 1.x is taken as 1.1.
        if (t.type != HttpTokenType::Version || t.len != 8 || std::memcmp(t.text, "HTTP/", 5) != 0 ||
            t.text[6] != '.' || t.text[5] < '0' || t.text[5] > '9' || t.text[7] < '0' || t.text[7] > '9') {
            return fail(400);
        }
        if (t.text[5] != '1') return fail(505);
        req_.minor_ = static_cast<uint8_t>(t.text[7] - '0');
        req_.headers_ = reinterpret_cast<const char*>(arena_ + used_);
        inName_ = true;
        st_ = St::Headers;
        return;

    case St::Headers:
        if (t.type == HttpTokenType::HeaderName && inName_) {
            if (!append(t.text, t.len, 431)) return;
            if (!t.more) { if (!endString(431)) return; inName_ = false; }
            return;
        }
        if (t.type == HttpTokenType::HeaderValue && !inName_) {
            if (!append(t.text, t.len, 431)) return;
            if (!t.more) { if (!endString(431)) return; inName_ = true; ++req_.headerCount_; }
            return;
        }
        if (t.type == HttpTokenType::HeadersEnd && inName_) {
            if (!headersDone(t.num)) return;
            st_ = St::Body;
            return;
        }
        return fail(400);

    case St::Body:
        if (t.type == HttpTokenType::Body) {
            // headersDone() made room for all of it.
            if (req_.bodyLen_ + t.len > size_ - used_ - 1) return fail(413);
            std::memcpy(arena_ + used_ + req_.bodyLen_, t.text, t.len);
            req_.bodyLen_ += t.len;
            return;
        }
        if (t.type == HttpTokenType::MessageEnd) {
            arena_[used_ + req_.bodyLen_] = 0;   // so a text body can be read as a string
            dispatch();
            return;
        }
        return fail(400);

    case St::Skip:
        return;
    }
}

bool HttpConnection::targetDone() {
    char* target = reinterpret_cast<char*>(arena_ + mark_);
    req_.target_ = target;
    // origin-form "/p?q", absolute-form "http://host/p?q", or "*" (OPTIONS).
    const char* p = target;
    if (*p != '/') {
        if (std::strcmp(p, "*") == 0) {
            req_.path_ = "*";
            return true;
        }
        const char* scheme = std::strstr(p, "://");
        if (scheme == nullptr || scheme == p) { fail(400); return false; }
        p = scheme + 3;
        while (*p && *p != '/' && *p != '?') ++p;
    }
    const char* q = std::strchr(p, '?');
    const size_t plen = q ? static_cast<size_t>(q - p) : std::strlen(p);
    req_.query_ = q ? q + 1 : "";
    if (plen == 0) {
        req_.path_ = "/";
        return true;
    }
    char* path = reinterpret_cast<char*>(arena_ + used_);
    const int32_t n = HttpRequest::urlDecode(p, plen, path, size_ - used_, false);
    if (n < 0) {
        // Either it didn't fit, or it decodes to a NUL.
        fail(plen + 1 > size_ - used_ ? 414 : 400);
        return false;
    }
    req_.path_ = path;
    used_ += static_cast<size_t>(n) + 1;
    return true;
}

bool HttpConnection::headersDone(uint32_t contentLength) {
    if (req_.minor_ >= 1 && req_.header("Host") == nullptr) { fail(400); return false; }
    const char* conn = req_.header("Connection");
    keepAlive_ = req_.minor_ >= 1 ? !(conn && hasToken(conn, "close")) : (conn && hasToken(conn, "keep-alive"));
    // All of the body, and a NUL after it.
    if (used_ + 1 > size_ || contentLength > size_ - used_ - 1) { fail(413); return false; }
    const char* expect = req_.header("Expect");
    if (expect) {
        if (!equalsNoCase(expect, "100-continue")) { fail(417); return false; }
        // The client waits for this before it sends the body.
        static const char kContinue[] = "HTTP/1.1 100 Continue\r\n\r\n";
        if (req_.minor_ >= 1 && contentLength > 0 && !out_.write(kContinue, sizeof kContinue - 1)) {
            done_ = true;
            return false;
        }
    }
    req_.body_ = arena_ + used_;
    req_.bodyLen_ = 0;
    return true;
}

void HttpConnection::dispatch() {
    st_ = St::Idle;
    HttpHandler fn = nullptr;
    void* ctx = nullptr;
    char allow[48];
    const uint16_t found = routes_.find(req_.method_, req_.path_, fn, ctx, allow, sizeof allow);
    if (found) return answer(found, allow);

    ++requests_;
    res_.reset(req_.http11(), keepAlive_ && requests_ < maxRequests_, req_.method_ == HttpMethod::Head);
    fn(req_, res_, ctx);
    if (!res_.started()) {
        // The handler answered nothing: a bug, and the client mustn't hang.
        res_.status(500).close();
        res_.sendStatus(500);
        ++errors_;
    } else if (!res_.ended()) {
        res_.end();
    }
    if (res_.failed() || !res_.keepAlive()) done_ = true;
}
