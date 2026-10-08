#pragma once
#include <cstddef>
#include <cstdint>
#include "HttpClientProtocol.h"
#include "HttpLexer.h"
#include "xClient.h"
#include "xNetInterface.h"

// An HTTP/1.1 client on any xNetInterface (xEthernet, xWifi), for
// FreeRTOS: a node sending its readings to a server, say. Each call runs
// on the calling thread and sleeps up to its timeout; there is no thread
// of its own and no allocation:
//
//     static xHttpClient http(eth);
//     char body[128];
//     int n = std::snprintf(body, sizeof body, "{\"t\":%.1f}", t);
//     uint8_t reply[256];
//     xHttpClient::Response r;
//     if (http.post("http://192.168.1.10:8080/api/readings", "application/json",
//                   body, n, reply, sizeof reply, r, 5000) == 201) ...
//
// The host may be a name (looked up with the interface's DNS) or
// "a.b.c.d". The connection is kept for the next request to the same
// host and port, unless either side says close. A request on a kept
// connection that the server had already closed is sent again on a new
// one, if it is a GET, HEAD, PUT, DELETE or OPTIONS; a POST is not
// repeated (it may have been acted on), and fails with Error::Closed.
//
// The response comes back framed by Content-Length, chunked (decoded) or
// by the connection closing, through HttpLexer in Response mode and
// HttpResponseReader. Its body goes into the caller's buffer
// (NUL-terminated when there is room), or piece by piece to onBody for
// bodies of any size.
//
// One thread per client, as for xClient. No TLS: https:// URLs fail with
// Error::Unsupported.
class xHttpClient {
public:
    struct Config {
        const char* userAgent = "iNetTransport";   // nullptr: none
        bool        keepAlive = true;
        uint32_t    resolveTimeoutMs = 5000;        // each also bounded by the call's timeout
        uint32_t    connectTimeoutMs = 5000;
    };

    enum class Error : uint8_t {
        None,
        BadUrl,        // not an http:// URL, or a host name over 63 characters
        Unsupported,   // https://
        Resolve,       // the host name didn't resolve
        Connect,       // refused, or no answer, or no socket free
        Send,          // the connection failed while sending
        Timeout,       // no complete response in time
        Closed,        // the server closed the connection before answering
        Protocol,      // the response was malformed
        Aborted,       // onBody returned false
    };

    struct Request {
        HttpMethod  method = HttpMethod::Get;
        const char* url = nullptr;
        const char* contentType = nullptr;
        const void* body = nullptr;
        size_t      bodyLength = 0;
        const char* headers = nullptr;            // extra "Name: value\r\n" lines
        // Where the response's body goes: into response (up to
        // responseCap bytes), or to onBody instead.
        uint8_t*    response = nullptr;
        size_t      responseCap = 0;
        HttpResponseReader::BodyFn   onBody = nullptr;
        HttpResponseReader::HeaderFn onHeader = nullptr;
        void*       ctx = nullptr;
    };

    struct Response {
        uint16_t status = 0;
        size_t   length = 0;          // body bytes received
        bool     truncated = false;   // more than responseCap: the rest was dropped
    };

    explicit xHttpClient(xNetInterface& net) : xHttpClient(net, Config()) {}
    xHttpClient(xNetInterface& net, const Config& cfg) : net_(net), cfg_(cfg), client_(net) {}

    // The response's status (100..599), or 0 on failure (see error()).
    uint16_t request(const Request& req, Response& res, uint32_t timeoutMs);

    uint16_t get(const char* url, uint8_t* response, size_t cap, Response& res, uint32_t timeoutMs);
    uint16_t post(const char* url, const char* contentType, const void* body, size_t len, uint8_t* response,
                  size_t cap, Response& res, uint32_t timeoutMs);

    Error error() const { return error_; }
    bool  connected() const { return open_; }   // a connection is kept
    void  close();                               // drop the kept connection

private:
    // The socket as an HttpOutput, within the call's deadline.
    struct Out : HttpOutput {
        xHttpClient* c = nullptr;
        bool write(const void* data, size_t len) override;
    };

    uint16_t fail(Error e) { error_ = e; return 0; }
    uint32_t left() const;   // ms to the call's deadline
    bool     send(const Request& req, const HttpUrl& u, const char* hostHeader);

    xNetInterface& net_;
    Config         cfg_;
    xClient        client_;
    HttpLexer      lexer_{HttpLexer::Mode::Response};
    HttpResponseReader reader_;
    Out            out_;
    Error          error_ = Error::None;

    bool       open_ = false;
    char       host_[64] = {};       // the kept connection's
    uint16_t   port_ = 0;
    TickType_t start_ = 0, total_ = 0;
    uint8_t    readBuf_[128];
};
