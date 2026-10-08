#pragma once
#include <cstddef>
#include <cstdint>
#include "HttpRequest.h"
#include "HttpResponse.h"
#include "HttpToken.h"

// The pure logic of an HTTP/1.1 client, used by xHttpClient: URLs, the
// request head, and reading the response from HttpLexer's tokens.

// "http://host[:port][/path][?query]". Pointers into the URL, not copies.
struct HttpUrl {
    bool        https = false;
    const char* host = nullptr;   size_t hostLen = 0;
    uint16_t    port = 80;
    const char* path = "/";       size_t pathLen = 1;   // with the query: what goes in the request line

    // false: not an http:// or https:// URL, no host, a bad port, or
    // characters a request line can't carry (spaces, controls).
    static bool parse(const char* url, HttpUrl& out);
};

// The request line and headers, to out. hostHeader is "host" or
// "host:port". contentLength < 0: no body and no Content-Length (GET);
// a POST or PUT always says its length, even 0. extraHeaders: complete
// "Name: value\r\n" lines, or nullptr.
bool httpWriteRequestHead(HttpOutput& out, HttpMethod method, const char* hostHeader, const char* path,
                          size_t pathLen, const char* contentType, int32_t contentLength,
                          const char* extraHeaders, const char* userAgent, bool keepAlive);

// The tokens of one response, from a lexer in Response mode: the status,
// the headers (to a callback, if wanted), and the body (into a buffer,
// or to a callback). 1xx responses are passed over. Always takes the
// token: put() never refuses, so the lexer can feed it directly.
class HttpResponseReader : public HttpTokenSink {
public:
    // A body piece, as it arrives. Return false to stop: the connection
    // is then closed.
    typedef bool (*BodyFn)(const uint8_t* data, size_t len, void* ctx);
    // A header. Names over 31 and values over 127 characters are cut there.
    typedef void (*HeaderFn)(const char* name, const char* value, void* ctx);

    static constexpr size_t kName = 32;
    static constexpr size_t kValue = 128;

    // Before each response. The body goes to onBody if given, else into
    // body (up to cap bytes; the rest is read and dropped: truncated()).
    void begin(uint8_t* body, size_t cap, BodyFn onBody, HeaderFn onHeader, void* ctx);

    bool put(const HttpToken& t) override;

    bool     done() const { return st_ == St::Done; }
    bool     failed() const { return st_ == St::Failed; }    // a malformed response
    bool     aborted() const { return st_ == St::Aborted; }  // onBody said stop
    bool     started() const { return st_ != St::Status; }   // any of it has arrived
    uint16_t status() const { return status_; }
    uint32_t contentLength() const { return length_; }       // or HttpToken::kChunked / kToClose
    size_t   bodyLength() const { return bodyLen_; }         // received (all of it, even when truncated)
    bool     truncated() const { return truncated_; }
    // Whether the connection can carry another request after this one.
    bool     keepAlive() const { return keepAlive_; }

private:
    enum class St : uint8_t { Status, Headers, Body, Done, Failed, Aborted };

    void headerDone();

    uint8_t* body_ = nullptr;
    size_t   cap_ = 0;
    BodyFn   onBody_ = nullptr;
    HeaderFn onHeader_ = nullptr;
    void*    ctx_ = nullptr;

    St       st_ = St::Status;
    uint16_t status_ = 0;
    bool     http11_ = true;
    bool     closeHeader_ = false, keepAliveHeader_ = false;
    bool     keepAlive_ = false;
    bool     truncated_ = false;
    uint32_t length_ = 0;
    size_t   bodyLen_ = 0;
    char     name_[kName];
    char     value_[kValue];
    size_t   nameLen_ = 0, valueLen_ = 0;
    bool     inValue_ = false;
};
