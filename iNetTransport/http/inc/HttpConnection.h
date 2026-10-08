#pragma once
#include <cstddef>
#include <cstdint>
#include "HttpRequest.h"
#include "HttpResponse.h"
#include "HttpToken.h"

typedef void (*HttpHandler)(const HttpRequest& req, HttpResponse& res, void* ctx);

// What answers which requests. A path is matched exactly, or as a
// prefix when it ends in '*' ("/api/*" matches "/api/" and everything
// under it). The first route that matches both path and method wins. A
// HEAD request goes to the GET route, without the body. Fill it in
// before the server starts; it is read-only after that.
class HttpRoutes {
public:
    static constexpr uint8_t kMaxRoutes = 16;

    bool add(HttpMethod method, const char* path, HttpHandler fn, void* ctx = nullptr);

    // The route for this request: 0 found, else 404 or 405 (allow: the
    // methods the path does take, for the Allow header).
    uint16_t find(HttpMethod method, const char* path, HttpHandler& fn, void*& ctx, char* allow, size_t allowCap) const;

    static bool pathMatches(const char* pattern, const char* path);

private:
    struct Route { HttpMethod method; const char* path; HttpHandler fn; void* ctx; };
    Route   routes_[kMaxRoutes];
    uint8_t n_ = 0;
};

// One client connection's requests, from tokens to responses: a state
// machine that takes the lexer's tokens one at a time, assembles each
// request in the arena, calls its handler when it is complete and makes
// sure it was answered. Requests that are malformed, too big or not
// routed are answered here (400, 404, 405, 413, 414, 431, 501, 505). Knows
// nothing of sockets or threads: the caller feeds it and closes the
// connection once done().
class HttpConnection {
public:
    HttpConnection(const HttpRoutes& routes, uint8_t* arena, size_t arenaSize, HttpOutput& out,
                   uint16_t maxRequests = 100);

    void reset();                     // a new connection
    void onToken(const HttpToken& t);
    // The request in progress didn't complete in time: 408, and done.
    void timeout();

    bool     done() const { return done_; }               // close the connection
    bool     midRequest() const { return st_ != St::Idle; }
    uint32_t requests() const { return requests_; }       // answered on this connection
    uint32_t errors() const { return errors_; }           // of those, answered here with 4xx/5xx

private:
    enum class St : uint8_t { Idle, Target, Version, Headers, Body, Skip };

    void beginRequest();
    void fail(uint16_t code);
    bool append(const char* s, size_t n, uint16_t codeIfFull);
    bool endString(uint16_t codeIfFull);
    bool targetDone();
    bool headersDone(uint32_t contentLength);
    void dispatch();
    void answer(uint16_t code, const char* allow = nullptr);

    const HttpRoutes& routes_;
    uint8_t*     arena_;
    size_t       size_;
    size_t       used_ = 0;
    size_t       mark_ = 0;          // where the string being appended began
    HttpResponse res_;
    HttpOutput&  out_;
    HttpRequest  req_;
    uint16_t     maxRequests_;

    St       st_ = St::Idle;
    bool     done_ = false;
    bool     inName_ = false;        // a header name is being appended
    bool     keepAlive_ = true;
    uint32_t requests_ = 0, errors_ = 0;
};
