#include "WebAuth.h"
#include <cstdio>
#include <cstring>
#include "HttpJson.h"

namespace {

const char kHex[] = "0123456789abcdef";

void toHex(const uint8_t* in, size_t n, char* out) {
    for (size_t i = 0; i < n; ++i) {
        out[2 * i] = kHex[in[i] >> 4];
        out[2 * i + 1] = kHex[in[i] & 15];
    }
    out[2 * n] = 0;
}

int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

// Equal, taking the same time however early they differ.
bool sameBytes(const void* a, const void* b, size_t n) {
    const uint8_t* x = static_cast<const uint8_t*>(a);
    const uint8_t* y = static_cast<const uint8_t*>(b);
    uint8_t d = 0;
    for (size_t i = 0; i < n; ++i) d |= static_cast<uint8_t>(x[i] ^ y[i]);
    return d == 0;
}

void jsonError(HttpResponse& res, uint16_t status, const char* message) {
    char body[96];
    std::snprintf(body, sizeof body, "{\"error\":\"%s\"}", message);
    res.status(status).header("Cache-Control", "no-store");
    res.send("application/json", body);
}

bool isJson(const HttpRequest& req) {
    const char* t = req.header("Content-Type");
    return t != nullptr && std::strncmp(t, "application/json", 16) == 0;
}

// A field from a JSON body or a form.
bool field(const HttpRequest& req, const char* name, char* out, size_t cap) {
    if (isJson(req)) {
        return HttpJson::string(reinterpret_cast<const char*>(req.body()), req.bodyLength(), name, out, cap);
    }
    return req.param(name, out, cap);
}

}  // namespace

WebAuth::WebAuth(const Config& cfg) : cfg_(cfg) {
    if (cfg_.userCount > kMaxUsers) cfg_.userCount = kMaxUsers;
    if (cfg_.maxFailures == 0) cfg_.maxFailures = 1;
}

const char* WebAuth::roleName(WebRole r) {
    switch (r) {
    case WebRole::Viewer:   return "viewer";
    case WebRole::Operator: return "operator";
    case WebRole::Admin:    return "admin";
    default:                return "none";
    }
}

int WebAuth::userIndex(const char* name) const {
    for (size_t i = 0; i < cfg_.userCount; ++i) {
        if (std::strcmp(cfg_.users[i].name, name) == 0) return static_cast<int>(i);
    }
    return -1;
}

size_t WebAuth::sessions() {
    lock();
    expireLocked(now());
    size_t n = 0;
    for (const Session& s : sessions_) n += s.used ? 1 : 0;
    unlock();
    return n;
}

void WebAuth::expireLocked(uint32_t nowMs) {
    for (Session& s : sessions_) {
        if (s.used && (nowMs - s.lastUsed >= cfg_.idleTimeoutMs || nowMs - s.created >= cfg_.maxSessionMs)) {
            std::memset(&s, 0, sizeof s);
        }
    }
}

// Compares with every session, so the time doesn't tell which matched.
int WebAuth::findLocked(const uint8_t* token, uint32_t nowMs) {
    expireLocked(nowMs);
    int found = -1;
    for (int i = 0; i < kMaxSessions; ++i) {
        const bool same = sameBytes(sessions_[i].token, token, kTokenBytes);
        if (sessions_[i].used && same) found = i;
    }
    return found;
}

// "sid=<64 hex>" from the Cookie header.
bool WebAuth::cookieToken(const HttpRequest& req, uint8_t* token) const {
    const char* c = req.header("Cookie");
    if (c == nullptr) return false;
    for (const char* p = c; (p = std::strstr(p, "sid=")) != nullptr; p += 4) {
        if (p != c && p[-1] != ' ' && p[-1] != ';') continue;   // "xsid=" is another cookie
        const char* v = p + 4;
        bool ok = true;
        for (size_t i = 0; i < kTokenBytes && ok; ++i) {
            const int hi = hexDigit(v[2 * i]), lo = hi < 0 ? -1 : hexDigit(v[2 * i + 1]);
            ok = hi >= 0 && lo >= 0;
            if (ok) token[i] = static_cast<uint8_t>(hi * 16 + lo);
        }
        if (ok && (v[2 * kTokenBytes] == 0 || v[2 * kTokenBytes] == ';' || v[2 * kTokenBytes] == ' ')) return true;
    }
    return false;
}

// An Origin header, when the browser sends one, must be this server.
bool WebAuth::originOk(const HttpRequest& req) const {
    const char* origin = req.header("Origin");
    if (origin == nullptr) return true;
    const char* host = req.header("Host");
    if (host == nullptr) return false;
    const char* scheme = req.secure() ? "https://" : "http://";
    const size_t n = std::strlen(scheme);
    return std::strncmp(origin, scheme, n) == 0 && std::strcmp(origin + n, host) == 0;
}

bool WebAuth::identify(const HttpRequest& req, Identity& who) {
    uint8_t token[kTokenBytes];
    if (!cookieToken(req, token)) return false;
    const uint32_t t = now();
    lock();
    const int i = findLocked(token, t);
    if (i >= 0) {
        Session& s = sessions_[i];
        s.lastUsed = t;
        const WebUser& u = cfg_.users[s.user];
        std::strncpy(who.user, u.name, kMaxName);
        who.user[kMaxName] = 0;
        who.role = u.role;
        std::memcpy(who.csrf, s.csrf, sizeof who.csrf);
    }
    unlock();
    return i >= 0;
}

bool WebAuth::require(const HttpRequest& req, HttpResponse& res, WebRole role, bool changesState, Identity* who) {
    Identity me;
    if (!identify(req, me)) {
        jsonError(res, 401, "login required");
        return false;
    }
    if (static_cast<uint8_t>(me.role) < static_cast<uint8_t>(role)) {
        jsonError(res, 403, "not allowed");
        return false;
    }
    if (changesState) {
        const char* csrf = req.header("X-CSRF-Token");
        if (csrf == nullptr || std::strlen(csrf) != 2 * kCsrfBytes || !sameBytes(csrf, me.csrf, 2 * kCsrfBytes)) {
            jsonError(res, 403, "missing or wrong CSRF token");
            return false;
        }
        if (!originOk(req)) {
            jsonError(res, 403, "cross-origin request");
            return false;
        }
    }
    if (who) *who = me;
    return true;
}

void WebAuth::login(const HttpRequest& req, HttpResponse& res, void* self) {
    WebAuth& a = *static_cast<WebAuth*>(self);
    if (a.cfg_.requireSecure && !req.secure()) return jsonError(res, 403, "log in over https");
    if (!a.originOk(req)) return jsonError(res, 403, "cross-origin request");
    if (a.cfg_.verify == nullptr || a.cfg_.random == nullptr || a.cfg_.userCount == 0) {
        return jsonError(res, 503, "logins are not set up");
    }
    char name[kMaxName + 1], password[kMaxPassword + 1];
    if (!field(req, "user", name, sizeof name) || !field(req, "password", password, sizeof password)) {
        return jsonError(res, 400, "user and password needed");
    }
    const int u = a.userIndex(name);

    // Locked out? (An unknown name is never locked: nothing to count against.)
    uint32_t t = a.now();
    if (u >= 0) {
        a.lock();
        const Failures f = a.failures_[u];
        a.unlock();
        if (f.count >= a.cfg_.maxFailures && static_cast<int32_t>(f.until - t) > 0) {
            char wait[12];
            std::snprintf(wait, sizeof wait, "%lu", static_cast<unsigned long>((f.until - t + 999) / 1000));
            res.header("Retry-After", wait);
            std::memset(password, 0, sizeof password);
            return jsonError(res, 429, "too many failed logins: try again later");
        }
    }

    // The slow part, outside the lock. An unknown name is checked against
    // the first user, so it takes as long, and always fails.
    const bool ok = a.cfg_.verify(a.cfg_.users[u >= 0 ? u : 0], password, a.cfg_.verifyCtx) && u >= 0;
    std::memset(password, 0, sizeof password);
    t = a.now();

    if (!ok) {
        if (u >= 0) {
            a.lock();
            Failures& f = a.failures_[u];
            if (f.count < 255) ++f.count;
            if (f.count >= a.cfg_.maxFailures) {
                uint32_t lock = a.cfg_.lockoutMs;
                for (uint8_t i = a.cfg_.maxFailures; i < f.count && lock < a.cfg_.maxLockoutMs; ++i) lock *= 2;
                if (lock > a.cfg_.maxLockoutMs) lock = a.cfg_.maxLockoutMs;
                f.until = t + lock;
            }
            a.unlock();
        }
        return jsonError(res, 401, "wrong user or password");
    }

    Session s;
    std::memset(&s, 0, sizeof s);
    uint8_t csrf[kCsrfBytes];
    if (!a.cfg_.random(s.token, kTokenBytes, a.cfg_.randomCtx) || !a.cfg_.random(csrf, kCsrfBytes, a.cfg_.randomCtx)) {
        return jsonError(res, 503, "no random numbers");
    }
    toHex(csrf, kCsrfBytes, s.csrf);
    s.used = true;
    s.user = static_cast<uint8_t>(u);
    s.created = s.lastUsed = t;

    a.lock();
    a.failures_[u] = Failures{0, 0};
    a.expireLocked(t);
    // A free slot, else the session unused for longest.
    int slot = 0;
    for (int i = 0; i < kMaxSessions; ++i) {
        if (!a.sessions_[i].used) { slot = i; break; }
        if (t - a.sessions_[i].lastUsed > t - a.sessions_[slot].lastUsed) slot = i;
    }
    a.sessions_[slot] = s;
    a.unlock();

    char cookie[2 * kTokenBytes + 96];
    char token[2 * kTokenBytes + 1];
    toHex(s.token, kTokenBytes, token);
    std::snprintf(cookie, sizeof cookie, "sid=%s; Path=/; HttpOnly; SameSite=Strict; Max-Age=%lu%s", token,
                  static_cast<unsigned long>(a.cfg_.maxSessionMs / 1000), req.secure() ? "; Secure" : "");
    char body[160];
    std::snprintf(body, sizeof body, "{\"user\":\"%s\",\"role\":\"%s\",\"csrf\":\"%s\"}", a.cfg_.users[u].name,
                  roleName(a.cfg_.users[u].role), s.csrf);
    res.header("Set-Cookie", cookie).header("Cache-Control", "no-store");
    res.send("application/json", body);
}

void WebAuth::logout(const HttpRequest& req, HttpResponse& res, void* self) {
    WebAuth& a = *static_cast<WebAuth*>(self);
    uint8_t token[kTokenBytes];
    if (a.cookieToken(req, token)) {
        a.lock();
        const int i = a.findLocked(token, a.now());
        if (i >= 0) std::memset(&a.sessions_[i], 0, sizeof a.sessions_[i]);
        a.unlock();
    }
    res.header("Set-Cookie", req.secure() ? "sid=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0; Secure"
                                          : "sid=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0");
    res.header("Cache-Control", "no-store");
    res.send("application/json", "{}");
}

void WebAuth::session(const HttpRequest& req, HttpResponse& res, void* self) {
    WebAuth& a = *static_cast<WebAuth*>(self);
    Identity who;
    if (!a.identify(req, who)) return jsonError(res, 401, "not logged in");
    char body[160];
    std::snprintf(body, sizeof body, "{\"user\":\"%s\",\"role\":\"%s\",\"csrf\":\"%s\"}", who.user, roleName(who.role),
                  who.csrf);
    res.header("Cache-Control", "no-store");
    res.send("application/json", body);
}
