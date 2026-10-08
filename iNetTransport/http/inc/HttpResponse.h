#pragma once
#include <cstdarg>
#include <cstddef>
#include <cstdint>

// Where a response goes: an xClient on the target.
class HttpOutput {
public:
    virtual ~HttpOutput() = default;
    virtual bool write(const void* data, size_t len) = 0;   // all of it, or false
};

// A handler's answer. Either all at once:
//
//     res.send("text/plain", "hello");
//     res.status(404).send("text/plain", "no such sensor");
//
// or streamed, when the length isn't known in advance (sent chunked to
// HTTP/1.1 clients; to HTTP/1.0 clients the connection closes after it):
//
//     res.header("Cache-Control", "no-store").begin("text/html");
//     res.print("<ul>");
//     for (...) res.printf("<li>%s: %.1f</li>", name, value);
//     res.end();
//
// Headers go before begin()/send(). Nothing is buffered beyond the
// headers: each write goes to the output.
class HttpResponse {
public:
    static constexpr int32_t kUnknownLength = -1;
    static constexpr size_t  kHeaderBytes = 256;   // header() lines, together

    HttpResponse(HttpOutput& out) : out_(out) {}

    // For HttpConnection: before each request.
    void reset(bool http11, bool keepAlive, bool head);

    HttpResponse& status(uint16_t code);
    HttpResponse& header(const char* name, const char* value);
    HttpResponse& close() { keepAlive_ = false; return *this; }   // close the connection after this

    bool send(const char* contentType, const void* body, size_t len);
    bool send(const char* contentType, const char* text);
    // A status with its reason phrase as a text/plain body.
    bool sendStatus(uint16_t code);

    bool begin(const char* contentType, int32_t contentLength = kUnknownLength);
    bool write(const void* data, size_t len);
    bool print(const char* text);
    bool printf(const char* fmt, ...) __attribute__((format(printf, 2, 3)));
    bool end();

    bool     started() const { return started_; }
    bool     ended() const { return ended_; }
    bool     failed() const { return failed_; }
    bool     keepAlive() const { return keepAlive_; }
    uint16_t code() const { return code_; }

    static const char* reason(uint16_t code);

private:
    bool raw(const void* data, size_t len);
    bool rawText(const char* s);

    HttpOutput& out_;
    uint16_t code_ = 200;
    bool     http11_ = true, keepAlive_ = true, head_ = false;
    bool     started_ = false, ended_ = false, failed_ = false, chunked_ = false;
    int32_t  left_ = 0;   // body bytes still due; kUnknownLength: until the connection closes
    char     headers_[kHeaderBytes];
    size_t   headersLen_ = 0;
};
