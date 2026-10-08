// Host test for HttpFileAdmin: uploads in pieces, commit, list, delete,
// through the real lexer, connection state machine and WebAuth (with a
// plain-text password check, as WebAuth_test), over a store kept in
// memory. Logins and roles, the CSRF token, paths, offsets, limits, the
// audit log, and the page.
//
//   g++ -std=c++14 -Wall -Wextra -Iinc -I../inc test/HttpFileAdmin_test.cpp src/*.cpp -o HttpFileAdmin_test
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <vector>
#include "HttpConnection.h"
#include "HttpFileAdmin.h"
#include "HttpJson.h"
#include "HttpLexer.h"

static int g_failures = 0;
static void check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failures;
}
static bool has(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

static uint32_t clockFn(void*) { return 1000; }
static bool randomFn(uint8_t* out, size_t n, void*) {
    for (size_t i = 0; i < n; ++i) out[i] = static_cast<uint8_t>(std::rand());
    return true;
}
static bool verifyFn(const WebUser& u, const char* pw, void*) {
    return std::strncmp(reinterpret_cast<const char*>(u.salt), pw, sizeof u.salt) == 0;
}
static WebUser user(const char* name, WebRole role, const char* pw) {
    WebUser u{};
    u.name = name;
    u.role = role;
    std::strncpy(reinterpret_cast<char*>(u.salt), pw, sizeof u.salt);
    return u;
}

// A store in memory, keeping the contract HttpFileStore states.
struct MemStore : HttpFileStore {
    std::map<std::string, std::string> files, parts;
    bool full = false;
    int readers = 0;          // clients reading "busyPath" now
    std::string busyPath, held;
    int heldOpens = 0;        // times hold() was on while a sleep passed
    bool append(const char* path, size_t offset, const uint8_t* data, size_t len) override {
        if (full) return false;
        std::string& p = parts[path];
        if (offset == 0) p.clear();
        if (p.size() != offset) return false;
        p.append(reinterpret_cast<const char*>(data), len);
        return true;
    }
    bool commit(const char* path, size_t size) override {
        if (busy(path)) return false;
        auto it = parts.find(path);
        if (it == parts.end() || it->second.size() != size) return false;
        files[path] = it->second;
        parts.erase(it);
        return true;
    }
    bool remove(const char* path) override { return !busy(path) && files.erase(path) == 1; }
    bool busy(const char* path) override { return readers > 0 && busyPath == path; }
    void hold(const char* path, bool on) override {
        if (on) held = path;
        else if (held == path) held.clear();
    }
    bool list(ListFn fn, void* ctx) override {
        for (auto& f : files) fn(f.first.c_str(), f.second.size(), ctx);
        return true;
    }
    bool space(uint64_t& total, uint64_t& free) override { total = 1000000; free = 900000; return true; }
};

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
    explicit Web(HttpRoutes& r) : routes(r) { conn.setSecure(true); }
    std::string request(const std::string& method, const std::string& target, const std::string& headers = "",
                        const std::string& body = "") {
        out.data.clear();
        conn.reset();
        lx.reset();
        std::string req = method + " " + target + " HTTP/1.1\r\nHost: plc\r\n" + headers;
        if (!body.empty() || method != "GET") req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        req += "\r\n" + body;
        lx.feed(reinterpret_cast<const uint8_t*>(req.data()), req.size(), sink);
        while (!sink.q.empty()) { conn.onToken(sink.q.front()); sink.q.pop_front(); }
        return out.data;
    }
    static std::string body(const std::string& r) {
        const size_t at = r.find("\r\n\r\n");
        return at == std::string::npos ? "" : r.substr(at + 4);
    }
    // The body, chunked or not.
    static std::string content(const std::string& r) {
        std::string b = body(r);
        if (header(r, "Transfer-Encoding") != "chunked") return b;
        std::string out;
        size_t at = 0;
        for (;;) {
            const size_t eol = b.find("\r\n", at);
            if (eol == std::string::npos) return "<bad chunks>";
            const size_t n = std::strtoul(b.c_str() + at, nullptr, 16);
            if (n == 0) return out;
            out += b.substr(eol + 2, n);
            at = eol + 2 + n + 2;
        }
    }
    static std::string header(const std::string& r, const std::string& name) {
        const size_t at = r.find("\r\n" + name + ": ");
        if (at == std::string::npos) return "";
        const size_t v = at + name.size() + 4;
        return r.substr(v, r.find("\r\n", v) - v);
    }
};

struct Session { std::string cookie, csrf; std::string hdr() const { return "Cookie: " + cookie + "\r\nX-CSRF-Token: " + csrf + "\r\n"; } };
static Session login(Web& w, const char* name, const char* pw) {
    const std::string r = w.request("POST", "/api/login", "Content-Type: application/json\r\n",
                                    std::string("{\"user\":\"") + name + "\",\"password\":\"" + pw + "\"}");
    Session s;
    const std::string sc = Web::header(r, "Set-Cookie");
    s.cookie = sc.substr(0, sc.find(';'));
    const std::string b = Web::body(r);
    char csrf[40];
    if (HttpJson::string(b.data(), b.size(), "csrf", csrf, sizeof csrf)) s.csrf = csrf;
    return s;
}

// The handler's sleep: each one, a reader finishes (unless they never do).
static MemStore* g_store = nullptr;
static bool g_readersStay = false;
static uint32_t g_slept = 0;
static void sleepFn(uint32_t ms, void*) {
    g_slept += ms;
    if (!g_store->held.empty()) ++g_store->heldOpens;
    if (!g_readersStay && g_store->readers > 0) --g_store->readers;
}

static std::vector<std::string> g_audit;
static void audit(const char* user, const char* action, const char* path, size_t size, void*) {
    g_audit.push_back(std::string(user) + " " + action + " " + path + " " + std::to_string(size));
}

// The page's own JavaScript, as text (checked with node --check by hand).
static std::string page() { return std::string(reinterpret_cast<const char*>(httpFileAdminPage.data), httpFileAdminPage.size); }

int main() {
    std::srand(5);
    const WebUser users[] = {user("adm", WebRole::Admin, "adm-pass"), user("ann", WebRole::Operator, "ann-pass")};
    WebAuth::Config ac;
    ac.users = users;
    ac.userCount = 2;
    ac.verify = verifyFn;
    ac.random = randomFn;
    ac.now = clockFn;
    WebAuth auth(ac);
    MemStore store;
    HttpFileAdmin::Config fc;
    fc.maxFileBytes = 5000;
    fc.pieceBytes = 1000;
    fc.audit = audit;
    HttpRoutes none;
    check(!HttpFileAdmin(store, fc).attach(none), "attach() without WebAuth: nothing added");
    fc.auth = &auth;
    HttpFileAdmin admin(store, fc);
    HttpRoutes routes;
    check(auth.attach(routes) && admin.attach(routes), "attach()");
    Web w(routes);

    std::printf("who may\n");
    const Session adm = login(w, "adm", "adm-pass"), ann = login(w, "ann", "ann-pass");
    check(adm.csrf.size() == 32 && ann.csrf.size() == 32, "two logins");
    check(has(w.request("GET", "/api/files"), "401"), "no login: 401");
    check(has(w.request("PUT", "/api/files/index.html?offset=0", "", "x"), "401"), "no login, an upload: 401");
    check(has(w.request("GET", "/api/files", ann.hdr()), "403"), "an operator: 403");
    check(has(w.request("PUT", "/api/files/index.html?offset=0", ann.hdr(), "x"), "403"), "an operator's upload: 403");
    check(has(w.request("PUT", "/api/files/index.html?offset=0", "Cookie: " + adm.cookie + "\r\n", "x"), "403"),
          "an admin without the CSRF token: 403");
    check(has(w.request("PUT", "/api/files/index.html?offset=0", adm.hdr() + "Origin: https://evil.example\r\n", "x"), "403"),
          "from another origin: 403");
    check(store.parts.empty() && g_audit.empty(), "nothing stored, nothing audited");

    std::printf("an upload\n");
    std::string r = w.request("GET", "/api/files", "Cookie: " + adm.cookie + "\r\n");
    check(has(r, "200 OK") && Web::content(r) == "{\"files\":[],\"ok\":true,\"total\":1000000,\"free\":900000,\"piece\":1000,\"max\":5000}" &&
              Web::header(r, "Cache-Control") == "no-store",
          "list: empty, with the space and limits, no CSRF needed to read");
    std::string html(2500, 0);
    for (size_t i = 0; i < html.size(); ++i) html[i] = static_cast<char>(i * 13);   // every byte value
    bool ok = true;
    for (size_t at = 0; at < html.size(); at += 1000) {
        r = w.request("PUT", "/api/files/index.html?offset=" + std::to_string(at), adm.hdr(), html.substr(at, 1000));
        ok = ok && has(r, "200 OK") && Web::body(r) == "{\"received\":" + std::to_string(std::min<size_t>(at + 1000, html.size())) + "}";
    }
    check(ok, "three pieces of raw bytes: received 1000, 2000, 2500");
    check(store.files.empty() && store.parts["/index.html"] == html, "held as the upload, not yet the file");
    check(has(w.request("PUT", "/api/files/index.html?offset=7", adm.hdr(), "x"), "409"), "the wrong offset: 409");
    check(has(w.request("POST", "/api/files/index.html?size=2499", adm.hdr()), "409"), "commit, the wrong size: 409");
    r = w.request("POST", "/api/files/index.html?size=2500", adm.hdr());
    check(has(r, "200 OK") && store.files["/index.html"] == html && store.parts.empty(), "commit: the file is in place");
    check(has(w.request("POST", "/api/files/index.html?size=2500", adm.hdr()), "409"), "commit again: nothing to commit");
    check(has(w.request("PUT", "/api/files/css/site.css?offset=0", adm.hdr(), "b{}"), "200") &&
              has(w.request("POST", "/api/files/css/site.css?size=3", adm.hdr()), "200"),
          "a file in a folder");
    r = Web::content(w.request("GET", "/api/files", "Cookie: " + adm.cookie + "\r\n"));
    check(has(r, "{\"files\":[{\"path\":\"/css/site.css\",\"size\":3},{\"path\":\"/index.html\",\"size\":2500}],"),
          "list: both files");

    std::printf("limits and paths\n");
    check(has(w.request("PUT", "/api/files/big.bin?offset=4500", adm.hdr(), std::string(501, 'x')), "413"),
          "past maxFileBytes: 413");
    check(has(w.request("PUT", "/api/files/big.bin?offset=0", adm.hdr(), std::string(1000, 'x')), "200") &&
              has(w.request("PUT", "/api/files/big.bin?offset=99999999", adm.hdr(), "x"), "400"),
          "an offset beyond the limit: 400");
    const char* bad[] = {"/api/files/?offset=0", "/api/files/dir/?offset=0", "/api/files/..%2Fsecret?offset=0",
                         "/api/files/a/../b?offset=0", "/api/files/.hidden?offset=0", "/api/files/a/.index.html.part?offset=0",
                         "/api/files/a%0Ab?offset=0", "/api/files/a%7Fb?offset=0", "/api/files/a\\b?offset=0",
                         "/api/files/index.html", "/api/files/index.html?offset=-1", "/api/files/index.html?offset=1x",
                         "/api/files/index.html?offset="};
    bool allBad = true;
    for (const char* b : bad) {
        const std::string rr = w.request("PUT", b, adm.hdr(), "x");
        if (!has(rr, "400") && !has(rr, "404")) { allBad = false; std::printf("    accepted: %s\n", b); }
    }
    check(allBad, "empty, folder, \"..\", hidden, control characters, backslash, no or bad offset: refused");
    const std::string longName = "/api/files/" + std::string(HttpStaticFiles::kMaxPath, 'a') + "?offset=0";
    check(has(w.request("PUT", longName, adm.hdr(), "x"), "400"), "a path longer than kMaxPath: 400");
    store.full = true;
    check(has(w.request("PUT", "/api/files/more.txt?offset=0", adm.hdr(), "x"), "409"), "the store full: 409");
    store.full = false;

    std::printf("delete, audit\n");
    check(has(w.request("DELETE", "/api/files/css/site.css", ann.hdr()), "403"), "an operator can't delete");
    check(has(w.request("DELETE", "/api/files/css/site.css", adm.hdr()), "200") && store.files.count("/css/site.css") == 0,
          "delete");
    check(has(w.request("DELETE", "/api/files/css/site.css", adm.hdr()), "404"), "delete again: 404");
    const std::vector<std::string> want = {"adm upload /index.html 0", "adm commit /index.html 2500",
                                           "adm upload /css/site.css 0", "adm commit /css/site.css 3",
                                           "adm upload /big.bin 0", "adm delete /css/site.css 0"};
    check(g_audit == want, "audit: who uploaded, committed, deleted what");
    if (g_audit != want) for (auto& a : g_audit) std::printf("    %s\n", a.c_str());

    std::printf("a file being read\n");
    {
        g_store = &store;
        store.busyPath = "/index.html";
        store.readers = 2;
        w.request("PUT", "/api/files/index.html?offset=0", adm.hdr(), "v3");
        std::string r = w.request("POST", "/api/files/index.html?size=2", adm.hdr());
        check(has(r, "503") && Web::header(r, "Retry-After") == "1" && store.files["/index.html"] == html,
              "no sleep hook: 503 with Retry-After, the old file kept");
        r = w.request("DELETE", "/api/files/index.html", adm.hdr());
        check(has(r, "503") && store.files.count("/index.html") == 1, "a delete too");

        HttpFileAdmin::Config wc = fc;
        wc.sleep = sleepFn;
        wc.busyWaitMs = 200;
        HttpFileAdmin waiting(store, wc);
        HttpRoutes wr;
        auth.attach(wr);
        waiting.attach(wr);
        Web ww(wr);
        r = ww.request("POST", "/api/files/index.html?size=2", adm.hdr());
        check(has(r, "200 OK") && store.files["/index.html"] == "v3" && g_slept == 20 && store.heldOpens == 2 &&
                  store.held.empty(),
              "with one: held closed to new readers, waited for the two to finish (20 ms), committed, released");
        store.readers = 1;
        g_readersStay = true;
        g_slept = 0;
        r = ww.request("DELETE", "/api/files/index.html", adm.hdr());
        check(has(r, "503") && g_slept == 200 && store.held.empty() && store.files.count("/index.html") == 1,
              "a reader that stays: busyWaitMs, then 503, and the hold released");
        g_readersStay = false;
        store.readers = 0;
    }

    std::printf("a name to escape\n");
    check(has(w.request("PUT", "/api/files/say%20%22hi%22.txt?offset=0", adm.hdr(), "x"), "200") &&
              has(w.request("POST", "/api/files/say%20%22hi%22.txt?size=1", adm.hdr()), "200") &&
              has(Web::content(w.request("GET", "/api/files", "Cookie: " + adm.cookie + "\r\n")), "{\"path\":\"/say \\\"hi\\\".txt\",\"size\":1}"),
          "quotes in a name come out escaped in the JSON");

    std::printf("the page\n");
    const std::string p = page();
    check(std::strcmp(httpFileAdminPage.path, "/files.html") == 0 && has(p, "<!doctype html>"), "/files.html");
    check(has(p, "X-CSRF-Token") && has(p, "offset=") && has(p, "size=") && has(p, "/api/login") && has(p, "piece"),
          "it logs in, slices by piece, sends the token and commits");
    check(!has(p, "innerHTML"), "no innerHTML: names are shown as text");

    std::printf("%s\n", g_failures ? "FAILED" : "all passed");
    return g_failures ? 1 : 0;
}
