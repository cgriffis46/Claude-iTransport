#pragma once
#include <cstdint>
#include "HttpConnection.h"

// Sends every request on to HTTPS: run it on a plain server on port 80,
// beside the HTTPS one, so a browser typing the board's address ends up
// on the secure site.
//
//     static HttpsRedirect toHttps(443);
//     plain.get("/*", HttpsRedirect::handler, &toHttps);
//
// 301 to https://<host><target>, where host is the request's Host
// (without its port) unless one is given, and the port is added unless
// it is 443.
class HttpsRedirect {
public:
    explicit HttpsRedirect(uint16_t httpsPort = 443, const char* host = nullptr) : port_(httpsPort), host_(host) {}
    static void handler(const HttpRequest& req, HttpResponse& res, void* self);

private:
    uint16_t    port_;
    const char* host_;
};
