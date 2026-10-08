#include "xHttpClient.h"
#include <cstdio>
#include <cstring>

uint32_t xHttpClient::left() const {
    const TickType_t used = xTaskGetTickCount() - start_;
    if (used >= total_) return 0;
    return static_cast<uint32_t>((total_ - used) * portTICK_PERIOD_MS);
}

static uint32_t atMost(uint32_t a, uint32_t b) { return a < b ? a : b; }

bool xHttpClient::Out::write(const void* data, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    while (len) {
        const uint32_t ms = c->left();
        if (ms == 0) return false;
        const int32_t w = c->client_.write(p, len, ms);
        if (w <= 0) return false;
        p += w;
        len -= static_cast<size_t>(w);
    }
    return true;
}

void xHttpClient::close() {
    if (!open_) return;
    client_.stop(1000);
    open_ = false;
}

uint16_t xHttpClient::get(const char* url, uint8_t* response, size_t cap, Response& res, uint32_t timeoutMs) {
    Request r;
    r.url = url;
    r.response = response;
    r.responseCap = cap;
    return request(r, res, timeoutMs);
}

uint16_t xHttpClient::post(const char* url, const char* contentType, const void* body, size_t len,
                           uint8_t* response, size_t cap, Response& res, uint32_t timeoutMs) {
    Request r;
    r.method = HttpMethod::Post;
    r.url = url;
    r.contentType = contentType;
    r.body = body;
    r.bodyLength = len;
    r.response = response;
    r.responseCap = cap;
    return request(r, res, timeoutMs);
}

bool xHttpClient::send(const Request& req, const HttpUrl& u, const char* hostHeader) {
    out_.c = this;
    const bool sendsLength = req.body != nullptr || req.method == HttpMethod::Post ||
                             req.method == HttpMethod::Put || req.method == HttpMethod::Patch;
    if (!httpWriteRequestHead(out_, req.method, hostHeader, u.path, u.pathLen, req.contentType,
                              sendsLength ? static_cast<int32_t>(req.bodyLength) : -1, req.headers,
                              cfg_.userAgent, cfg_.keepAlive)) {
        return false;
    }
    return req.bodyLength == 0 || out_.write(req.body, req.bodyLength);
}

uint16_t xHttpClient::request(const Request& req, Response& res, uint32_t timeoutMs) {
    res = Response();
    error_ = Error::None;
    start_ = xTaskGetTickCount();
    total_ = xNetInterface::toTicks(timeoutMs);

    HttpUrl u;
    if (req.method == HttpMethod::Other || !HttpUrl::parse(req.url, u)) return fail(Error::BadUrl);
    if (u.https) return fail(Error::Unsupported);
    if (u.hostLen >= sizeof host_) return fail(Error::BadUrl);
    char host[sizeof host_];
    std::memcpy(host, u.host, u.hostLen);
    host[u.hostLen] = 0;
    char hostHeader[sizeof host_ + 6];
    if (u.port == 80) std::snprintf(hostHeader, sizeof hostHeader, "%s", host);
    else std::snprintf(hostHeader, sizeof hostHeader, "%s:%u", host, u.port);
    if (req.bodyLength && req.body == nullptr) return fail(Error::BadUrl);

    const bool idempotent = req.method != HttpMethod::Post && req.method != HttpMethod::Patch;

    for (int attempt = 0; attempt < 2; ++attempt) {
        // Keep the connection only for the same server, and only while it is up.
        if (open_ && (std::strcmp(host_, host) != 0 || port_ != u.port || !client_.connected())) close();
        const bool reused = open_;
        if (!open_) {
            IpAddress ip;
            if (!net_.resolve(host, ip, atMost(cfg_.resolveTimeoutMs, left()))) return fail(Error::Resolve);
            if (!client_.connect(ip, u.port, atMost(cfg_.connectTimeoutMs, left()))) {
                client_.stop(100);   // give the socket back
                return fail(left() ? Error::Connect : Error::Timeout);
            }
            open_ = true;
            std::memcpy(host_, host, u.hostLen + 1);
            port_ = u.port;
        }

        if (!send(req, u, hostHeader)) {
            close();
            if (reused && idempotent && left()) continue;
            return fail(left() ? Error::Send : Error::Timeout);
        }

        lexer_.reset();
        if (req.method == HttpMethod::Head) lexer_.expectNoBody();
        reader_.begin(req.response, req.responseCap, req.onBody, req.onHeader, req.ctx);

        bool closed = false;
        while (!reader_.done() && !reader_.failed() && !reader_.aborted()) {
            const uint32_t ms = left();
            if (ms == 0) {
                close();
                return fail(Error::Timeout);
            }
            const int32_t n = client_.read(readBuf_, sizeof readBuf_, ms);
            if (n < 0) {
                // The server closed: the end of a body that runs to the
                // close, or a response cut short.
                closed = true;
                lexer_.endOfInput();
                lexer_.feed(nullptr, 0, reader_);
                break;
            }
            // The reader takes every token, so the lexer takes every byte.
            lexer_.feed(readBuf_, static_cast<size_t>(n), reader_);
        }

        if (closed && !reader_.started()) {
            // Closed without a word: on a kept connection the server had
            // probably dropped it while idle.
            close();
            if (reused && idempotent && left()) continue;
            return fail(Error::Closed);
        }
        if (reader_.aborted()) { close(); return fail(Error::Aborted); }
        if (!reader_.done()) { close(); return fail(closed ? Error::Closed : Error::Protocol); }

        res.status = reader_.status();
        res.length = reader_.bodyLength();
        res.truncated = reader_.truncated();
        if (closed || !reader_.keepAlive() || !cfg_.keepAlive) close();
        return res.status;
    }
    return fail(Error::Closed);
}
