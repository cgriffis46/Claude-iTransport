// Host test for WebAuth and HttpJson: logins, sessions, roles, the CSRF
// token and Origin check, expiry, lockout, and the cookie's attributes,
// through the real lexer and connection state machine, on a simulated
// clock, with a plain-text password check standing in for PBKDF2 (which
// tls/'s tests check against Python's). Then many threads at once, for
// TSan.
//
//   g++ -std=c++14 -Wall -Wextra -pthread -Iinc test/WebAuth_test.cpp src/*.cpp -o WebAuth_test
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "HttpConnection.h"
#include "HttpJson.h"
#include "HttpLexer.h"
#include "WebAuth.h"

static int g_failures = 0;
static void check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failures;
}
static bool has(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

// ---- the things WebAuth is given ----
static std::atomic<uint32_t> g_now{1000};
static uint32_t clockFn(void*) { return g_now; }
static bool randomFn(uint8_t* out, size_t n, void*) {
    for (size_t i = 0; i < n; ++i) out[i] = static_cast<uint8_t>(std::rand());
    return true;
}
static std::atomic<int> g_verifies{0};
// The "hash" is the password itself, in the salt field: a stand-in.
static bool verifyFn(const WebUser& u, const char* pw, void*) {
    ++g_verifies;
    return std::strncmp(reinterpret_cast<const char*>(u.salt), pw, sizeof u.salt) == 0 && std::strlen(pw) < sizeof u.salt;
}
static WebUser user(const char* name, WebRole role, const char* pw) {
    WebUser u{};
    u.name = name;
    u.role = role;
    std::strncpy(reinterpret_cast<char*>(u.salt), pw, sizeof u.salt);
    return u;
}
struct StdLock : WebAuth::Lock {
    std::mutex m;
    void lock() override { m.lock(); }
    void unlock() override { m.unlock(); }
};

// ---- a connection ----
struct Out : HttpOutput {
    std::string data;
    bool write(const void* d, size_t n) override { data.append(static_cast<const char*>(d), n); return true; }
};
struct Sink : HttpTokenSink {
    std::deque<HttpToken> q;
    bool put(const HttpToken& t) override { q.push_back(t); return true; }
};
struct Web {
    HttpRoutes& routes;
    uint8_t arena[2048];
    Out out;
    HttpConnection conn{routes, arena, sizeof arena, out};
    HttpLexer lx;
    Sink sink;
    explicit Web(HttpRoutes& r, bool secure = true) : routes(r) { conn.setSecure(secure); }
    std::string request(const std::string& method, const std::string& target, const std::string& headers = "",
                        const std::string& body = "") {
        out.data.clear();
        conn.reset();
        lx.reset();
        std::string req = method + " " + target + " HTTP/1.1\r\nHost: plc\r\n" + headers;
        if (!body.empty() || method == "POST") req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        req += "\r\n" + body;
        lx.feed(reinterpret_cast<const uint8_t*>(req.data()), req.size(), sink);
        while (!sink.q.empty()) { conn.onToken(sink.q.front()); sink.q.pop_front(); }
        return out.data;
    }
    static std::string body(const std::string& r) {
        const size_t at = r.find("\r\n\r\n");
        return at == std::string::npos ? "" : r.substr(at + 4);
    }
    static std::string header(const std::string& r, const std::string& name) {
        const size_t at = r.find("\r\n" + name + ": ");
        if (at == std::string::npos) return "";
        const size_t v = at + name.size() + 4;
        return r.substr(v, r.find("\r\n", v) - v);
    }
};

static const char kForm[] = "Content-Type: application/x-www-form-urlencoded\r\n";

static std::string cookieOf(const std::string& resp) {
    const std::string sc = Web::header(resp, "Set-Cookie");
    return sc.substr(0, sc.find(';'));   // "sid=..."
}
static std::string csrfOf(const std::string& resp) {
    const std::string b = Web::body(resp);
    char csrf[40];
    return HttpJson::string(b.data(), b.size(), "csrf", csrf, sizeof csrf) ? csrf : "";
}

// A handler that needs an operator, and changes something.
static WebAuth* g_auth = nullptr;
static void setpoint(const HttpRequest& req, HttpResponse& res, void*) {
    WebAuth::Identity who;
    if (!g_auth->require(req, res, WebRole::Operator, true, &who)) return;
    res.send("text/plain", (std::string("set by ") + who.user).c_str());
}
static void readings(const HttpRequest& req, HttpResponse& res, void*) {
    if (!g_auth->require(req, res, WebRole::Viewer, false)) return;
    res.send("text/plain", "readings");
}

int main() {
    std::srand(3);
    std::printf("HttpJson\n");
    {
        const char* j = " { \"user\" : \"ann\\u00e9\\n\", \"n\":-12.5e3,\"b\":true, \"o\":{\"x\":[1,{\"y\":2}]},"
                        "\"esc\\\"aped\":1, \"s\":\"\\ud83d\\ude00\", \"last\":null } ";
        const size_t n = std::strlen(j);
        char buf[32];
        const char* v;
        size_t len;
        check(HttpJson::string(j, n, "user", buf, sizeof buf) && std::strcmp(buf, "ann\xc3\xa9\n") == 0,
              "a string, unescaped, \\u as UTF-8");
        check(HttpJson::raw(j, n, "n", v, len) && std::string(v, len) == "-12.5e3", "a number's raw text");
        check(HttpJson::raw(j, n, "b", v, len) && std::string(v, len) == "true", "true");
        check(HttpJson::raw(j, n, "last", v, len) && std::string(v, len) == "null", "after a nested object and array");
        check(HttpJson::string(j, n, "s", buf, sizeof buf) && std::strcmp(buf, "\xf0\x9f\x98\x80") == 0, "a surrogate pair");
        check(!HttpJson::raw(j, n, "x", v, len), "nested members aren't top-level");
        check(!HttpJson::raw(j, n, "missing", v, len), "missing");
        check(!HttpJson::string(j, n, "n", buf, sizeof buf), "a number isn't a string");
        check(!HttpJson::string(j, n, "user", buf, 4), "too long for the buffer");
        const char* bad[] = {"", "[]", "{", "{\"a\":}", "{\"a\" 1}", "{\"a\":1,}", "{\"a\":\"x}", "{\"a\":\"\\u0000\"}",
                             "{\"a\":\"\x01\"}"};
        bool all = true;
        for (const char* b : bad) all &= !HttpJson::string(b, std::strlen(b), "a", buf, sizeof buf);
        check(all, "malformed: nothing");
        std::string deep = "{\"a\":";
        for (int i = 0; i < 40; ++i) deep += "[";
        check(!HttpJson::raw(deep.data(), deep.size(), "a", v, len), "deep nesting refused");
    }

    const WebUser users[] = {user("ann", WebRole::Operator, "ann-pass"), user("vic", WebRole::Viewer, "vic-pass"),
                             user("adm", WebRole::Admin, "adm-pass")};
    StdLock lock;
    WebAuth::Config c;
    c.users = users;
    c.userCount = 3;
    c.verify = verifyFn;
    c.random = randomFn;
    c.now = clockFn;
    c.lock = &lock;
    WebAuth auth(c);
    g_auth = &auth;
    HttpRoutes routes;
    check(auth.attach(routes), "attach()");
    routes.on(HttpMethod::Post, "/api/setpoint", setpoint);
    routes.on(HttpMethod::Get, "/api/readings", readings);
    Web w(routes);

    std::printf("login\n");
    {
        std::string r = w.request("POST", "/api/login", "Content-Type: application/json\r\n",
                                  "{\"user\":\"ann\",\"password\":\"ann-pass\"}");
        check(has(r, "HTTP/1.1 200 OK"), "JSON login: 200");
        const std::string sc = Web::header(r, "Set-Cookie");
        check(sc.compare(0, 4, "sid=") == 0 && sc.find(';') == 4 + 64 &&
                  has(sc, "; Path=/; HttpOnly; SameSite=Strict; Max-Age=28800; Secure"),
              "cookie: 32 random bytes, HttpOnly, SameSite=Strict, Secure, 8 h");
        check(has(Web::body(r), "\"user\":\"ann\",\"role\":\"operator\",\"csrf\":\"") && csrfOf(r).size() == 32,
              "body: user, role, a CSRF token");
        check(Web::header(r, "Cache-Control") == "no-store", "not cached");
        const std::string cookie = cookieOf(r), csrf = csrfOf(r);

        r = w.request("GET", "/api/session", "Cookie: theme=dark; " + cookie + "\r\n");
        check(has(r, "200 OK") && csrfOf(r) == csrf, "/api/session: the same user and token, among other cookies");
        check(has(w.request("GET", "/api/session"), "401"), "no cookie: 401");
        check(has(w.request("GET", "/api/session", "Cookie: sid=" + std::string(64, '0') + "\r\n"), "401"),
              "a made-up token: 401");
        check(has(w.request("GET", "/api/session", "Cookie: xsid=" + cookie.substr(4) + "\r\n"), "401"),
              "the token under another cookie's name: 401");

        r = w.request("POST", "/api/login", "Content-Type: application/x-www-form-urlencoded\r\n",
                      "user=vic&password=vic-pass");
        check(has(r, "200 OK") && has(r, "\"role\":\"viewer\""), "form login");
        const std::string vicCookie = cookieOf(r), vicCsrf = csrfOf(r);
        check(csrf != vicCsrf && cookie != vicCookie, "each session its own token");

        std::printf("roles, CSRF, Origin\n");
        const std::string hdr = "Cookie: " + cookie + "\r\nX-CSRF-Token: " + csrf + "\r\n";
        check(Web::body(w.request("POST", "/api/setpoint", hdr)) == "set by ann", "operator with its token: allowed");
        check(has(w.request("POST", "/api/setpoint"), "401") && has(w.request("POST", "/api/setpoint"), "login required"),
              "no session: 401");
        check(has(w.request("POST", "/api/setpoint", "Cookie: " + cookie + "\r\n"), "403"), "no CSRF token: 403");
        check(has(w.request("POST", "/api/setpoint", "Cookie: " + cookie + "\r\nX-CSRF-Token: " + vicCsrf + "\r\n"),
                  "403"),
              "another session's token: 403");
        check(has(w.request("POST", "/api/setpoint", hdr + "Origin: https://evil.example\r\n"), "cross-origin"),
              "a foreign Origin: 403");
        check(Web::body(w.request("POST", "/api/setpoint", hdr + "Origin: https://plc\r\n")) == "set by ann",
              "our own Origin: allowed");
        check(has(w.request("POST", "/api/setpoint", "Cookie: " + vicCookie + "\r\nX-CSRF-Token: " + vicCsrf + "\r\n"),
                  "403 ") && has(w.request("POST", "/api/setpoint", "Cookie: " + vicCookie + "\r\nX-CSRF-Token: " + vicCsrf + "\r\n"), "not allowed"),
              "a viewer can't: 403");
        check(Web::body(w.request("GET", "/api/readings", "Cookie: " + vicCookie + "\r\n")) == "readings",
              "a viewer can read, no token needed for a GET");

        std::printf("expiry and logout\n");
        g_now += 14 * 60 * 1000;
        check(has(w.request("GET", "/api/session", "Cookie: " + cookie + "\r\n"), "200"), "14 min idle: still on");
        g_now += 14 * 60 * 1000;
        check(has(w.request("GET", "/api/session", "Cookie: " + cookie + "\r\n"), "200"), "used again: idle time restarts");
        check(has(w.request("GET", "/api/session", "Cookie: " + vicCookie + "\r\n"), "401"),
              "28 min idle (the viewer): expired");
        for (int i = 0; i < 40; ++i) {   // in use every 14 minutes, for over 8 hours
            g_now += 14 * 60 * 1000;
            w.request("GET", "/api/session", "Cookie: " + cookie + "\r\n");
        }
        check(has(w.request("GET", "/api/session", "Cookie: " + cookie + "\r\n"), "401"), "8 hours in all: expired");

        r = w.request("POST", "/api/login", "Content-Type: application/json\r\n", "{\"user\":\"adm\",\"password\":\"adm-pass\"}");
        const std::string admCookie = cookieOf(r);
        r = w.request("POST", "/api/logout", "Cookie: " + admCookie + "\r\n");
        check(has(r, "200") && has(Web::header(r, "Set-Cookie"), "sid=; ") && has(Web::header(r, "Set-Cookie"), "Max-Age=0"),
              "logout clears the cookie");
        check(has(w.request("GET", "/api/session", "Cookie: " + admCookie + "\r\n"), "401"), "and ends the session");
    }

    std::printf("wrong passwords\n");
    {
        const int before = g_verifies;
        std::string r = w.request("POST", "/api/login", "Content-Type: application/json\r\n", "{\"user\":\"ann\",\"password\":\"nope\"}");
        check(has(r, "401") && has(r, "wrong user or password"), "wrong password: 401");
        r = w.request("POST", "/api/login", "Content-Type: application/json\r\n", "{\"user\":\"nobody\",\"password\":\"nope\"}");
        check(has(r, "401") && has(r, "wrong user or password") && g_verifies == before + 2,
              "unknown user: the same answer, after the same password check");
        check(has(w.request("POST", "/api/login", "Content-Type: application/json\r\n", "{\"user\":\"ann\"}"), "400"),
              "no password: 400");
        for (int i = 0; i < 3; ++i) w.request("POST", "/api/login", kForm, "user=ann&password=x");
        r = w.request("POST", "/api/login", kForm, "user=ann&password=x");
        check(has(r, "401"), "the fifth wrong password: still 401, and now locked");
        r = w.request("POST", "/api/login", kForm, "user=ann&password=x");
        check(has(r, "429") && Web::header(r, "Retry-After") == "60", "after 5 failures: 429, Retry-After 60");
        const int v = g_verifies;
        r = w.request("POST", "/api/login", kForm, "user=ann&password=ann-pass");
        check(has(r, "429") && g_verifies == v, "locked out: even the right password, and it isn't checked");
        check(has(w.request("POST", "/api/login", kForm, "user=vic&password=vic-pass"), "200"), "other users aren't locked");
        g_now += 61 * 1000;
        r = w.request("POST", "/api/login", kForm, "user=ann&password=x");
        check(has(r, "401"), "a minute later: tried again, wrong");
        r = w.request("POST", "/api/login", kForm, "user=ann&password=x");
        check(has(r, "429") && Web::header(r, "Retry-After") == "120", "and locked out longer: 2 minutes");
        g_now += 121 * 1000;
        check(has(w.request("POST", "/api/login", kForm, "user=ann&password=ann-pass"), "200"), "then the right one works");
        check(has(w.request("POST", "/api/login", kForm, "user=ann&password=x"), "401") &&
                  has(w.request("POST", "/api/login", kForm, "user=ann&password=x"), "401"),
              "and the count started again");
    }

    std::printf("over plain HTTP\n");
    {
        Web plain(routes, false);
        const std::string r = plain.request("POST", "/api/login", kForm, "user=ann&password=ann-pass");
        check(has(r, "403") && has(r, "log in over https") && Web::header(r, "Set-Cookie").empty(),
              "a login over plain HTTP: refused, no cookie");
        WebAuth::Config c2 = c;
        c2.requireSecure = false;
        WebAuth relaxed(c2);
        HttpRoutes r2;
        relaxed.attach(r2);
        Web plain2(r2, false);
        const std::string ok = plain2.request("POST", "/api/login", kForm, "user=vic&password=vic-pass");
        check(has(ok, "200") && !has(Web::header(ok, "Set-Cookie"), "Secure"), "allowed if configured, without Secure");
        check(has(w.request("POST", "/api/login", std::string(kForm) + "Origin: https://evil.example\r\n", "user=vic&password=vic-pass"), "403"),
              "a login from another site's page: 403");
    }

    std::printf("sessions full\n");
    {
        std::vector<std::string> cookies;
        for (int i = 0; i < WebAuth::kMaxSessions + 2; ++i) {
            g_now += 10;
            cookies.push_back(cookieOf(w.request("POST", "/api/login", kForm, "user=vic&password=vic-pass")));
        }
        check(auth.sessions() == WebAuth::kMaxSessions, "at most kMaxSessions");
        check(has(w.request("GET", "/api/session", "Cookie: " + cookies[0] + "\r\n"), "401") &&
                  has(w.request("GET", "/api/session", "Cookie: " + cookies.back() + "\r\n"), "200"),
              "the least recently used made way");
        // All in the same millisecond: a session just made is the newest,
        // not (as a wrapped subtraction once had it) the oldest.
        const std::string a1 = cookieOf(w.request("POST", "/api/login", kForm, "user=vic&password=vic-pass"));
        const std::string a2 = cookieOf(w.request("POST", "/api/login", kForm, "user=vic&password=vic-pass"));
        check(has(w.request("GET", "/api/session", "Cookie: " + a1 + "\r\n"), "200") &&
                  has(w.request("GET", "/api/session", "Cookie: " + a2 + "\r\n"), "200"),
              "logins in the same millisecond don't evict each other");
    }

    std::printf("threads\n");
    {
        g_now += 1000;   // the sessions left above are older than any made here
        std::atomic<int> ok{0}, bad{0};
        std::vector<std::thread> ts;
        for (int t = 0; t < 8; ++t) {
            ts.emplace_back([&, t] {
                Web mine(routes);
                for (int i = 0; i < 50; ++i) {
                    const std::string r = mine.request("POST", "/api/login", kForm, t % 2 ? "user=ann&password=ann-pass"
                                                                                         : "user=vic&password=vic-pass");
                    const std::string ck = cookieOf(r), tok = csrfOf(r);
                    const std::string s = mine.request("POST", "/api/setpoint", "Cookie: " + ck + "\r\nX-CSRF-Token: " + tok + "\r\n");
                    mine.request("POST", "/api/logout", "Cookie: " + ck + "\r\n");   // or the 8 slots fill
                    if (t % 2 ? has(s, "set by ann") : has(s, "403")) ++ok;
                    else ++bad;
                }
            });
        }
        for (auto& t : ts) t.join();
        check(ok == 400 && bad == 0, "8 threads, 400 logins and requests, each answered right");
    }

    std::printf("%s\n", g_failures ? "FAILED" : "all passed");
    return g_failures ? 1 : 0;
}
