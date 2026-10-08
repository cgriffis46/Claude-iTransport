// Host test for PlcTagWebApi: the PLC tag database as JSON, through the
// real HttpLexer and HttpConnection, with a registry holding a tag of
// every type. Also checks that a slow client doesn't hold the registry's
// lock while its response is written.
//
//   g++ -std=c++14 -Wall -Wextra -pthread -I../inc -Iinc -I<iNetTransport>/http/inc
//       test/PlcTagWebApi_test.cpp src/*.cpp <iNetTransport>/http/src/*.cpp -o PlcTagWebApi_test
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <limits>
#include <string>
#include <thread>
#include "HttpConnection.h"
#include "HttpFiles.h"
#include "HttpLexer.h"
#include "PlcTagWebApi.h"
#include "PlcWebDefaultPage.h"
#include "HttpJson.h"
#include "WebAuth.h"

static int g_failures = 0;
static void check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failures;
}
static bool has(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

struct Out : HttpOutput {
    std::string data;
    std::function<void()> onWrite;
    bool write(const void* d, size_t n) override {
        if (onWrite) onWrite();
        data.append(static_cast<const char*>(d), n);
        return true;
    }
};

struct Sink : HttpTokenSink {
    std::deque<HttpToken> q;
    bool put(const HttpToken& t) override { q.push_back(t); return true; }
};

// One connection: a request in, the response's body out (chunks joined).
struct Web {
    HttpRoutes routes;
    uint8_t arena[1024];
    Out out;
    HttpConnection conn{routes, arena, sizeof arena, out};
    HttpLexer lx;
    Sink sink;

    std::string get(const std::string& target) { return request("GET", target); }
    std::string request(const std::string& method, const std::string& target, const std::string& headers = "",
                        const std::string& body = "") {
        out.data.clear();
        conn.reset();
        lx.reset();
        std::string req = method + " " + target + " HTTP/1.1\r\nHost: plc\r\n" + headers;
        if (method == "POST") req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        req += "\r\n" + body;
        lx.feed(reinterpret_cast<const uint8_t*>(req.data()), req.size(), sink);
        while (!sink.q.empty()) { conn.onToken(sink.q.front()); sink.q.pop_front(); }
        return out.data;
    }
    // The body of a chunked or plain response.
    static std::string body(const std::string& r) {
        const size_t at = r.find("\r\n\r\n");
        if (at == std::string::npos) return "";
        std::string b = r.substr(at + 4);
        if (!has(r, "Transfer-Encoding: chunked")) return b;
        std::string joined;
        size_t p = 0;
        for (;;) {
            const size_t eol = b.find("\r\n", p);
            const size_t n = std::stoul(b.substr(p, eol - p), nullptr, 16);
            if (n == 0) break;
            joined += b.substr(eol + 2, n);
            p = eol + 2 + n + 2;
        }
        return joined;
    }
};

struct Motor { float speed; int32_t faults; uint8_t mode; };

// WebAuth's inputs: a plain-text password check standing in for PBKDF2.
static uint32_t clockMs(void*) { return 1000; }
static bool rnd(uint8_t* out, size_t n, void*) {
    for (size_t i = 0; i < n; ++i) out[i] = static_cast<uint8_t>(std::rand());
    return true;
}
static bool plainVerify(const WebUser& u, const char* pw, void*) {
    return std::strcmp(reinterpret_cast<const char*>(u.salt), pw) == 0;
}
static WebUser user(const char* name, WebRole role, const char* pw) {
    WebUser u{};
    u.name = name;
    u.role = role;
    std::strncpy(reinterpret_cast<char*>(u.salt), pw, sizeof u.salt - 1);
    return u;
}
struct Audit {
    std::string log;
    static void fn(const char* who, const PlcTagSnapshot& before, const PlcTagSnapshot& after, void* ctx) {
        char b[80], a[80];
        PlcTagWebApi::formatValue(before, b, sizeof b);
        PlcTagWebApi::formatValue(after, a, sizeof a);
        static_cast<Audit*>(ctx)->log += std::string(who) + " " + before.name + " " + b + "->" + a + ";";
    }
};

int main() {
    PlcTagRegistry reg;
    bool run = true;
    int8_t sint = -5;
    int16_t i16 = -1234;
    int32_t dint = 2000000000;
    int64_t lint = std::numeric_limits<int64_t>::min();
    uint8_t usint = 200;
    uint16_t uint = 65535;
    uint32_t udint = 4000000000u;
    uint64_t ulint = 18446744073709551615ull;
    float real = 21.5f;
    double lreal = 0.1;
    float nan = std::numeric_limits<float>::quiet_NaN();
    Motor motor = {1500.5f, 3, 2};
    uint8_t big[100];
    for (int i = 0; i < 100; ++i) big[i] = static_cast<uint8_t>(i);
    reg.registerTag("Run", run, false);
    reg.registerTag("Sint", sint);
    reg.registerTag("Int", i16);
    reg.registerTag("Dint", dint);
    reg.registerTag("Lint", lint);
    reg.registerTag("Usint", usint);
    reg.registerTag("Uint", uint);
    reg.registerTag("Udint", udint);
    reg.registerTag("Ulint", ulint);
    reg.registerTag("Temp", real);
    reg.registerTag("Lreal", lreal);
    reg.registerTag("Broken", nan);
    reg.registerStructTag("Motor1", motor);
    reg.registerStructTag("Big", big);
    reg.registerTag("Odd \"name\"\\", dint);

    std::printf("JSON\n");
    {
        PlcTagWebApi api(reg);
        Web w;
        check(api.attach(w.routes), "attach() adds the routes");

        std::string r = w.get("/api/tags");
        check(has(r, "HTTP/1.1 200 OK\r\n") && has(r, "Content-Type: application/json\r\n") &&
                  has(r, "Cache-Control: no-store\r\n"),
              "200, JSON, not cached");
        const std::string b = Web::body(r);
        check(b.compare(0, 9, "{\"tags\":[") == 0 && b.compare(b.size() - 2, 2, "]}") == 0, "{\"tags\":[...]}");
        const char* want[] = {
            "{\"name\":\"Run\",\"type\":\"BOOL\",\"value\":true,\"writable\":false}",
            "{\"name\":\"Sint\",\"type\":\"SINT\",\"value\":-5,\"writable\":true}",
            "{\"name\":\"Int\",\"type\":\"INT\",\"value\":-1234,\"writable\":true}",
            "{\"name\":\"Dint\",\"type\":\"DINT\",\"value\":2000000000,\"writable\":true}",
            "{\"name\":\"Lint\",\"type\":\"LINT\",\"value\":-9223372036854775808,\"writable\":true}",
            "{\"name\":\"Usint\",\"type\":\"USINT\",\"value\":200,\"writable\":true}",
            "{\"name\":\"Uint\",\"type\":\"UINT\",\"value\":65535,\"writable\":true}",
            "{\"name\":\"Udint\",\"type\":\"UDINT\",\"value\":4000000000,\"writable\":true}",
            "{\"name\":\"Ulint\",\"type\":\"ULINT\",\"value\":18446744073709551615,\"writable\":true}",
            "{\"name\":\"Temp\",\"type\":\"REAL\",\"value\":21.5,\"writable\":true}",
            "{\"name\":\"Lreal\",\"type\":\"LREAL\",\"value\":0.10000000000000001,\"writable\":true}",
            "{\"name\":\"Broken\",\"type\":\"REAL\",\"value\":null,\"writable\":true}",
            "{\"name\":\"Odd \\\"name\\\"\\\\\",\"type\":\"DINT\",\"value\":2000000000,\"writable\":true}",
        };
        bool all = true;
        for (const char* t : want) {
            if (!has(b, t)) { std::printf("    missing %s\n", t); all = false; }
        }
        check(all, "every elementary type, NaN as null, names escaped");
        char hex[40];
        const uint8_t* m = reinterpret_cast<const uint8_t*>(&motor);
        for (size_t i = 0; i < sizeof motor; ++i) std::snprintf(hex + 2 * i, 3, "%02x", m[i]);
        check(has(b, std::string("{\"name\":\"Motor1\",\"type\":\"STRUCT\",\"value\":\"") + hex + "\",\"size\":" +
                         std::to_string(sizeof motor) + ",\"writable\":true}"),
              "a STRUCT as hex, with its size");
        check(has(b, "\"name\":\"Big\",\"type\":\"STRUCT\",\"value\":\"000102") && has(b, "3e3f\",\"size\":100,\"truncated\":true"),
              "a STRUCT over 64 bytes: its first 64, truncated");
        size_t n = 0;
        for (size_t at = 0; (at = b.find("{\"name\"", at)) != std::string::npos; ++at) ++n;
        check(n == reg.tagCount(), "every tag, once");

        r = Web::body(w.get("/api/tags?names=Temp,Nope,Run"));
        check(r == "{\"tags\":[{\"name\":\"Temp\",\"type\":\"REAL\",\"value\":21.5,\"writable\":true},"
                   "{\"name\":\"Nope\",\"error\":\"no such tag\"},"
                   "{\"name\":\"Run\",\"type\":\"BOOL\",\"value\":true,\"writable\":false}]}",
              "?names=: those, in order, the unknown one marked");
        check(Web::body(w.get("/api/tags?names=Odd%20%22name%22%5C")).find("\"value\":2000000000") != std::string::npos,
              "names percent-encoded");
        check(has(w.get("/api/tags?names=" + std::string(300, 'x')), "414"), "a names list too long: 414");

        r = w.get("/api/tags/Temp");
        check(Web::body(r) == "{\"name\":\"Temp\",\"type\":\"REAL\",\"value\":21.5,\"writable\":true}", "/api/tags/<name>");
        r = w.get("/api/tags/Nope");
        check(has(r, "404 Not Found") && Web::body(r) == "{\"name\":\"Nope\",\"error\":\"no such tag\"}", "an unknown tag: 404");
        check(has(w.get("/api/tags/"), "404 Not Found"), "no name: 404");

        real = -3.25f;
        check(has(Web::body(w.get("/api/tags/Temp")), "\"value\":-3.25"), "live: the value now");

        // Writes aren't routed: read-only.
        HttpRoutes& routes = w.routes;
        HttpHandler fn;
        void* ctx;
        char allow[48];
        check(routes.find(HttpMethod::Post, "/api/tags/Temp", fn, ctx, allow, sizeof allow) == 405 &&
                  std::string(allow) == "GET, HEAD",
              "POST and PUT: 405, read-only");
    }

    std::printf("the lock\n");
    {
        // A client that writes slowly: the registry stays usable meanwhile.
        PlcTagWebApi api(reg);
        Web w;
        api.attach(w.routes);
        std::atomic<int> blocked{0};
        w.out.onWrite = [&] {
            // A writer elsewhere (the control program, the CIP server)
            // must get the lock while the response is being written.
            std::atomic<bool> done{false};
            std::thread t([&] { int32_t v = 7; reg.writeTag("Dint", &v, sizeof v); done = true; });
            const auto t0 = std::chrono::steady_clock::now();
            while (!done && std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(500)) std::this_thread::yield();
            if (!done) ++blocked;
            t.join();
        };
        w.get("/api/tags");
        check(blocked == 0, "the lock is free whenever the response is being written");
        check(dint == 7, "and the writes went through");
    }

    std::printf("writes\n");
    {
        const WebUser users[] = {user("op", WebRole::Operator, "op-pass"), user("view", WebRole::Viewer, "view-pass")};
        WebAuth::Config ac;
        ac.users = users;
        ac.userCount = 2;
        ac.verify = plainVerify;
        ac.random = rnd;
        ac.now = clockMs;
        WebAuth auth(ac);
        const PlcWebWritable allowed[] = {
            {"Temp", -20, 80}, {"Run", 0, 0}, {"Dint", -100, 1000}, {"Lint", -1e19, 1e19}, {"Ulint", 0, 2e19},
            {"Int", -50, 50}, {"Usint", 0, 255}, {"Motor1", 0, 1}, {"Sint", -128, 127}, {"Lreal", -1, 1},
        };
        Audit audit;
        PlcTagWebApi::Config pc;
        pc.auth = &auth;
        pc.writable = allowed;
        pc.writableCount = sizeof allowed / sizeof allowed[0];
        pc.audit = Audit::fn;
        pc.auditCtx = &audit;
        PlcTagWebApi api(reg, pc);
        Web w;
        w.conn.setSecure(true);
        check(api.attach(w.routes) && auth.attach(w.routes), "routes, with the POST");

        auto login = [&](const char* u, const char* p, std::string& cookie, std::string& csrf) {
            const std::string r = w.request("POST", "/api/login", "Content-Type: application/json\r\n",
                                            std::string("{\"user\":\"") + u + "\",\"password\":\"" + p + "\"}");
            const size_t at = r.find("Set-Cookie: ");
            cookie = r.substr(at + 12, r.find(';', at) - at - 12);
            const std::string b = Web::body(r);
            char tok[40] = "";
            HttpJson::string(b.data(), b.size(), "csrf", tok, sizeof tok);
            csrf = tok;
        };
        std::string ck, tok, vck, vtok;
        login("op", "op-pass", ck, tok);
        login("view", "view-pass", vck, vtok);
        const std::string op = "Cookie: " + ck + "\r\nX-CSRF-Token: " + tok + "\r\nContent-Type: application/json\r\n";
        auto post = [&](const std::string& name, const std::string& body, const std::string& hdr) {
            return w.request("POST", "/api/tags/" + name, hdr, body);
        };

        real = 21.5f;
        std::string r = post("Temp", "{\"value\":42.25}", op);
        check(has(r, "200 OK") && real == 42.25f, "an operator writes a REAL");
        check(has(Web::body(r), "\"value\":42.25,\"writable\":true,\"webWritable\":true,\"min\":-20,\"max\":80"),
              "the answer: the tag now, with its limits");
        check(audit.log == "op Temp 21.5->42.25;", "audited: who, what, from, to");

        check(has(post("Temp", "{\"value\":81}", op), "422") && real == 42.25f, "above the list's limit: 422, unchanged");
        check(has(post("Temp", "{\"value\":\"hot\"}", op), "422"), "not a number: 422");
        check(has(post("Temp", "{\"nothing\":1}", op), "400"), "no value: 400");
        check(has(post("Temp", "{\"value\":1}", "Cookie: " + ck + "\r\nContent-Type: application/json\r\n"), "403") &&
                  real == 42.25f,
              "no CSRF token: 403, unchanged");
        check(has(post("Temp", "{\"value\":1}", "Content-Type: application/json\r\n"), "401"), "no login: 401");
        check(has(post("Temp", "{\"value\":1}", "Cookie: " + vck + "\r\nX-CSRF-Token: " + vtok +
                                                     "\r\nContent-Type: application/json\r\n"), "403") && real == 42.25f,
              "a viewer: 403, unchanged");
        check(has(post("Udint", "{\"value\":1}", op), "not writable from the web"), "not on the list: 403");
        check(has(post("Run", "{\"value\":false}", op), "tag is read-only"), "on the list but read-only in the registry: 403");
        check(has(post("Motor1", "{\"value\":1}", op), "tag is read-only"), "a STRUCT: 403");
        check(has(post("Nope", "{\"value\":1}", op), "404"), "no such tag: 404");

        check(has(post("Dint", "{\"value\":-100}", op), "200") && dint == -100, "DINT at its lower limit");
        check(has(post("Dint", "{\"value\":12.5}", op), "422") && dint == -100, "DINT with a fraction: 422");
        check(has(post("Int", "{\"value\":-50}", op), "200") && i16 == -50, "INT");
        check(has(post("Sint", "{\"value\":-129}", op), "422"), "SINT below -128: 422 (its type)");
        check(has(post("Usint", "{\"value\":-1}", op), "422") && has(post("Usint", "{\"value\":255}", op), "200") &&
                  usint == 255,
              "USINT: no negatives, 255 fine");
        check(has(post("Lint", "{\"value\":-9223372036854775807}", op), "200") && lint == -9223372036854775807LL,
              "LINT: all 64 bits, exactly");
        check(has(post("Ulint", "{\"value\":18446744073709551615}", op), "200") && ulint == 18446744073709551615ull,
              "ULINT: the largest");
        check(has(post("Ulint", "{\"value\":18446744073709551616}", op), "422"), "ULINT overflow: 422");
        check(has(post("Lreal", "{\"value\":-0.5}", op), "200") && lreal == -0.5, "LREAL");
        check(has(post("Temp", "value=12", "Cookie: " + ck + "\r\nX-CSRF-Token: " + tok +
                                        "\r\nContent-Type: application/x-www-form-urlencoded\r\n"), "200") &&
                  real == 12.0f,
              "a form works too");

        const std::string list = Web::body(w.get("/api/tags"));
        check(has(list, "\"name\":\"Temp\",\"type\":\"REAL\",\"value\":12,\"writable\":true,\"webWritable\":true") &&
                  has(list, "{\"name\":\"Udint\",\"type\":\"UDINT\",\"value\":4000000000,\"writable\":true}"),
              "GET marks the web-writable tags only");

        PlcTagWebApi::Config locked = pc;
        locked.readRole = WebRole::Viewer;
        PlcTagWebApi privateApi(reg, locked);
        Web w2;
        w2.conn.setSecure(true);
        privateApi.attach(w2.routes);
        check(has(w2.get("/api/tags"), "401"), "readRole Viewer: no login, no tags");
        check(has(w2.request("GET", "/api/tags", "Cookie: " + vck + "\r\n"), "200 OK"), "a viewer's session: the tags");

        PlcTagWebApi readOnly(reg);
        Web w3;
        readOnly.attach(w3.routes);
        check(has(w3.request("POST", "/api/tags/Temp", "Content-Type: application/json\r\n", "{\"value\":1}"), "405"),
              "no WebAuth: no POST route at all");
    }

    std::printf("default page\n");
    {
        HttpMemoryFiles builtIn(plcWebDefaultFiles, plcWebDefaultFileCount);
        HttpStaticFiles site(builtIn);
        Web w;
        w.routes.on(HttpMethod::Get, "/*", HttpStaticFiles::handler, &site);
        const std::string r = w.get("/");
        check(has(r, "200 OK") && has(r, "text/html") && has(r, "fetch('/api/tags'"), "/ serves the built-in page");
    }

    std::printf("%s\n", g_failures ? "FAILED" : "all passed");
    return g_failures ? 1 : 0;
}
