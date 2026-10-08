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

    std::string get(const std::string& target) {
        out.data.clear();
        conn.reset();
        lx.reset();
        const std::string req = "GET " + target + " HTTP/1.1\r\nHost: plc\r\n\r\n";
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
