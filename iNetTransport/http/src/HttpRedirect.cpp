#include "HttpRedirect.h"
#include <cstdio>
#include <cstring>

void HttpsRedirect::handler(const HttpRequest& req, HttpResponse& res, void* self) {
    const HttpsRedirect& me = *static_cast<const HttpsRedirect*>(self);
    char host[96];
    const char* h = me.host_ ? me.host_ : req.header("Host");
    if (h == nullptr || *h == 0) {
        res.status(400).send("text/plain", "Host needed");
        return;
    }
    // Without the port it came in on; an IPv6 literal keeps its brackets.
    size_t n = std::strlen(h);
    const char* colon = std::strrchr(h, ':');
    if (colon && (h[0] != '[' || colon > std::strchr(h, ']'))) n = static_cast<size_t>(colon - h);
    if (n >= sizeof host) {
        res.status(400).send("text/plain", "Host too long");
        return;
    }
    std::memcpy(host, h, n);
    host[n] = 0;
    // It must fit HttpResponse's header space: a target too long for it
    // goes to the site's root instead.
    char location[200];
    const char* target = req.target();
    if (target[0] != '/') target = "/";   // an absolute-form or * target: the site's root
    char port[8] = "";
    if (me.port_ != 443) std::snprintf(port, sizeof port, ":%u", me.port_);
    const int len = std::snprintf(location, sizeof location, "https://%s%s%s", host, port, target);
    if (len < 0 || static_cast<size_t>(len) >= sizeof location) {
        std::snprintf(location, sizeof location, "https://%s%s/", host, port);
    }
    res.status(301).header("Location", location);
    res.send("text/plain", "Moved to HTTPS");
}
