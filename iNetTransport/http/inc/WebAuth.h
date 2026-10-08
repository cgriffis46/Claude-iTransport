#pragma once
#include <cstddef>
#include <cstdint>
#include "HttpConnection.h"

// Roles, lowest first: each can do what those below it can.
enum class WebRole : uint8_t { None = 0, Viewer = 1, Operator = 2, Admin = 3 };

// A web user. The password is never kept, only its PBKDF2-HMAC-SHA256
// hash (tools/web_user.py makes these entries):
//   {"ann", WebRole::Operator, 20000, {salt...}, {hash...}}
struct WebUser {
    const char* name;
    WebRole     role;
    uint32_t    iterations;
    uint8_t     salt[16];
    uint8_t     hash[32];
};

// Logins and sessions for a web server:
//
//   POST /api/login    {"user":"ann","password":"..."} (or a form) ->
//                      200 {"user":"ann","role":"operator","csrf":"..."} and a
//                      session cookie; 401 wrong; 429 too many tries (Retry-After)
//   POST /api/logout   ends the session
//   GET  /api/session  who is logged in, and the CSRF token; 401 if no one
//
// A handler that needs a login asks require(). For anything that changes
// state, the request must also carry the session's CSRF token in an
// X-CSRF-Token header, and an Origin header, if any, must be this
// server's own.
//
// The session cookie is 32 random bytes: HttpOnly (page scripts can't read
// it), SameSite=Strict (other sites' pages can't send it), and Secure over
// HTTPS. By default a login is refused unless it came over HTTPS
// (Config::requireSecure), as a password sent in the clear can be read by
// anyone on the network.
//
// Sessions end after idleTimeoutMs without use, after maxSessionMs in
// all, or at logout. When kMaxSessions are open, a new login replaces the
// one used least recently. After maxFailures wrong passwords in a row, a
// user is locked out for lockoutMs, doubling with each further failure up
// to maxLockoutMs. A login for an unknown name costs as much as a wrong
// password, so names can't be found by timing.
//
// Thread-safe through Config::lock: the server calls it from each
// client's thread. The password check (slow by design) runs outside the
// lock.
class WebAuth {
public:
    static constexpr uint8_t kMaxSessions = 8;
    static constexpr uint8_t kMaxUsers = 16;
    static constexpr size_t  kTokenBytes = 32;
    static constexpr size_t  kCsrfBytes = 16;
    static constexpr size_t  kMaxName = 31;
    static constexpr size_t  kMaxPassword = 127;

    class Lock {
    public:
        virtual ~Lock() = default;
        virtual void lock() = 0;
        virtual void unlock() = 0;
    };
    typedef bool (*RandomFn)(uint8_t* out, size_t len, void* ctx);          // cryptographic quality
    typedef uint32_t (*ClockFn)(void* ctx);                                 // a ms counter
    typedef bool (*VerifyFn)(const WebUser& user, const char* password, void* ctx);   // tls/WebPassword: PBKDF2

    struct Config {
        const WebUser* users = nullptr;
        size_t         userCount = 0;       // up to kMaxUsers
        VerifyFn       verify = nullptr;
        void*          verifyCtx = nullptr;
        RandomFn       random = nullptr;
        void*          randomCtx = nullptr;
        ClockFn        now = nullptr;
        void*          nowCtx = nullptr;
        Lock*          lock = nullptr;
        uint32_t       idleTimeoutMs = 15u * 60u * 1000u;
        uint32_t       maxSessionMs = 8u * 3600u * 1000u;
        uint8_t        maxFailures = 5;
        uint32_t       lockoutMs = 60u * 1000u;
        uint32_t       maxLockoutMs = 15u * 60u * 1000u;
        bool           requireSecure = true;   // logins over HTTPS only
    };

    struct Identity {
        char    user[kMaxName + 1];
        WebRole role;
        char    csrf[2 * kCsrfBytes + 1];
    };

    WebAuth() : WebAuth(Config()) {}
    explicit WebAuth(const Config& cfg);

    // Adds the three routes to an xHttpServer or HttpRoutes.
    template <typename Server>
    bool attach(Server& server) {
        return server.on(HttpMethod::Post, "/api/login", login, this) &&
               server.on(HttpMethod::Post, "/api/logout", logout, this) &&
               server.on(HttpMethod::Get, "/api/session", session, this);
    }

    // For a handler: true if the request's session has at least role (and,
    // when changesState, the CSRF token and a matching Origin). Otherwise
    // it answers 401 or 403 itself and returns false. who: the user.
    bool require(const HttpRequest& req, HttpResponse& res, WebRole role, bool changesState,
                 Identity* who = nullptr);
    // The request's user, if it has a live session; no answer is sent.
    bool identify(const HttpRequest& req, Identity& who);

    static void login(const HttpRequest& req, HttpResponse& res, void* self);
    static void logout(const HttpRequest& req, HttpResponse& res, void* self);
    static void session(const HttpRequest& req, HttpResponse& res, void* self);

    static const char* roleName(WebRole r);
    size_t sessions();   // open now

private:
    struct Session {
        bool     used;
        uint8_t  token[kTokenBytes];
        char     csrf[2 * kCsrfBytes + 1];
        uint8_t  user;
        uint32_t created, lastUsed;
    };
    struct Failures {
        uint8_t  count;
        uint32_t until;   // locked until this ms (when count >= maxFailures)
    };

    void     lock() { if (cfg_.lock) cfg_.lock->lock(); }
    void     unlock() { if (cfg_.lock) cfg_.lock->unlock(); }
    uint32_t now() const { return cfg_.now ? cfg_.now(cfg_.nowCtx) : 0; }
    int      findLocked(const uint8_t* token, uint32_t nowMs);   // -1: none
    void     expireLocked(uint32_t nowMs);
    int      userIndex(const char* name) const;
    bool     cookieToken(const HttpRequest& req, uint8_t* token) const;
    bool     originOk(const HttpRequest& req) const;

    Config   cfg_;
    Session  sessions_[kMaxSessions] = {};
    Failures failures_[kMaxUsers] = {};
};
