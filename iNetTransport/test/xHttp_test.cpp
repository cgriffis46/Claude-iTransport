// Host test for xHttpServer: the daemon thread, a thread per client and
// the W5500's driver thread run for real on std::threads against the
// FreeRTOS simulation in stub/ (xTaskCreate starts a std::thread). The
// simulated chip's far end plays the browsers: it connects to the
// listening socket, sends requests, and collects what the server sends.
//
//   g++ -std=c++17 -Wall -Wextra -pthread -Istub -Isim -I../inc -I../hw/freertos/inc
//       -I../w5500/inc -I../http/inc -I../dhcp/inc -I../dns/inc -I../sntp/inc -I../mqtt/inc
//       -I<iTransport>/itransport/inc xHttp_test.cpp ../hw/freertos/src/*.cpp
//       ../w5500/src/*.cpp ../http/src/*.cpp ... -o xHttp_test
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "xEthernet.h"
#include "xHttpServer.h"
#include "W5500.h"
#include "SimW5500.h"

using namespace std::chrono;

static int g_failures = 0;
static void check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failures;
}

static long msSince(steady_clock::time_point t0) {
    return static_cast<long>(duration_cast<milliseconds>(steady_clock::now() - t0).count());
}
static void sleepMs(int ms) { std::this_thread::sleep_for(milliseconds(ms)); }

template <typename F>
static bool eventually(F f, int ms = 2000) {
    const auto t0 = steady_clock::now();
    while (msSince(t0) < ms) {
        if (f()) return true;
        sleepMs(2);
    }
    return f();
}

static bool has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }
static int count(const std::string& s, const char* part) {
    int n = 0;
    for (size_t at = 0; (at = s.find(part, at)) != std::string::npos; ++at) ++n;
    return n;
}

typedef W5500::w5500<FakeW5500Spi> Chip;

// ---- handlers ----

static void hello(const HttpRequest&, HttpResponse& res, void*) { res.send("text/plain", "hello"); }

static std::atomic<int> g_inSlow{0}, g_maxInSlow{0};
static void slow(const HttpRequest&, HttpResponse& res, void*) {
    const int now = ++g_inSlow;
    int m = g_maxInSlow;
    while (now > m && !g_maxInSlow.compare_exchange_weak(m, now)) {
    }
    sleepMs(300);   // a handler that blocks: only its own client waits
    --g_inSlow;
    res.send("text/plain", "slow");
}

static void form(const HttpRequest& req, HttpResponse& res, void*) {
    char led[8], level[8];
    if (!req.param("led", led, sizeof led) || !req.param("level", level, sizeof level)) {
        res.status(422).send("text/plain", "missing");
        return;
    }
    res.begin("application/json");
    res.printf("{\"led\":\"%s\",\"level\":%s}", led, level);
}

static void big(const HttpRequest&, HttpResponse& res, void*) {
    res.begin("text/plain");
    for (int i = 0; i < 400; ++i) res.printf("line %03d of a long streamed page\n", i);
}

// A W5500 behind an xEthernet with its driver thread, an xHttpServer
// with its daemon thread, and the browsers' side of the chip.
struct Rig {
    std::unique_ptr<SimW5500> sim{new SimW5500};
    std::unique_ptr<Chip> chip;
    std::unique_ptr<xEthernet> eth;
    std::unique_ptr<xHttpServer> web;
    std::atomic<bool> quit{false};
    std::thread driver, daemon;
    bool running = false;

    std::mutex m;                          // received, and one browser connecting at a time
    std::map<int, std::string> received;   // by socket

    static xHttpServer::Config defaults() {
        xHttpServer::Config c;
        c.maxClients = 3;
        c.requestBytes = 1024;
        return c;
    }

    explicit Rig(xHttpServer::Config cfg = defaults()) {
        chip.reset(new Chip(W5500::w5500_param_t(), *sim));
        eth.reset(new xEthernet(*chip));
        NetConfig net;
        net.mac = MacAddress(0x02, 0, 0, 0x12, 0x34, 0x56);
        net.ip = IpAddress(192, 168, 1, 50);
        net.subnet = IpAddress(255, 255, 255, 0);
        net.gateway = IpAddress(192, 168, 1, 1);
        net.dhcp = false;
        sim->onTcpSend = [this](uint8_t s, std::vector<uint8_t> data) {
            std::lock_guard<std::mutex> g(m);
            received[s].append(data.begin(), data.end());
        };
        eth->begin(net);
        driver = std::thread([this] { while (!quit) eth->service(20); });
        web.reset(new xHttpServer(*eth, cfg));
        web->get("/", hello);
        web->get("/slow", slow);
        web->post("/led", form);
        web->get("/big", big);
    }

    bool start() {
        if (!web->begin()) return false;
        daemon = std::thread([this] { web->run(); });
        running = true;
        return true;
    }

    void stopServer() {
        if (!running) return;
        web->stop();
        daemon.join();
        running = false;
    }

    ~Rig() {
        stopServer();
        web.reset();
        quit = true;
        driver.join();
    }

    int listening() {
        for (uint8_t s = 0; s < 8; ++s) if (sim->status(s) == W5500::w5500_SOCK_LISTEN) return s;
        return -1;
    }
    std::string got(int s) { std::lock_guard<std::mutex> g(m); return received[s]; }
};

// A browser: a TCP connection from the network to the server.
struct Browser {
    Rig& r;
    int s = -1;
    explicit Browser(Rig& rig) : r(rig) {}

    bool connect(int ms = 2000) {
        std::lock_guard<std::mutex> one(connectLock());
        int at = -1;
        if (!eventually([&] { return (at = r.listening()) >= 0; }, ms)) return false;
        {
            std::lock_guard<std::mutex> g(r.m);
            r.received[at].clear();
        }
        s = at;
        r.sim->peerConnect(static_cast<uint8_t>(s));
        // Wait for the daemon to take it, so the next browser finds the next socket.
        return eventually([&] { return r.listening() != s; }, ms);
    }
    void send(const std::string& d) {
        size_t at = 0;
        while (at < d.size()) {
            at += r.sim->peerSend(static_cast<uint8_t>(s), reinterpret_cast<const uint8_t*>(d.data()) + at, d.size() - at);
            if (at < d.size()) sleepMs(1);
        }
    }
    std::string got() { return r.got(s); }
    bool waitFor(const char* part, int ms = 2000) { return eventually([&] { return has(got(), part); }, ms); }
    bool closedByServer(int ms = 2000) {
        return eventually([&] { return r.sim->status(static_cast<uint8_t>(s)) == W5500::w5500_SOCK_CLOSED; }, ms);
    }
    void close() { r.sim->peerClose(static_cast<uint8_t>(s)); }

    static std::mutex& connectLock() { static std::mutex l; return l; }
};

static const char kGet[] = "GET / HTTP/1.1\r\nHost: dev\r\n\r\n";

int main() {
    std::printf("requests\n");
    {
        Rig r;
        check(r.start(), "begin()");
        check(eventually([&] { return r.listening() >= 0; }), "the daemon listens");
        const int created = simrtos::tasksCreated();
        Browser b(r);
        check(b.connect(), "a browser connects");
        check(eventually([&] { return simrtos::tasksCreated() == created + 1; }), "a thread is created for it");
        check(eventually([&] { return r.listening() >= 0; }), "and the daemon listens again");

        b.send(kGet);
        check(b.waitFor("hello"), "GET answered");
        check(has(b.got(), "HTTP/1.1 200 OK\r\n") && has(b.got(), "Content-Length: 5\r\n"), "200, with its length");

        b.send(std::string(kGet) + "GET /nope HTTP/1.1\r\nHost: dev\r\n\r\n" + kGet);
        check(eventually([&] { return count(b.got(), "hello") == 3 && has(b.got(), "404 Not Found"); }),
              "pipelined on the same connection, answered in order");

        // A form posted in pieces, slowly.
        const std::string body = "led=red&level=75";
        b.send("POST /led HTTP/1.1\r\nHost: dev\r\nContent-Type: application/x-www-form-urlencoded\r\n");
        sleepMs(30);
        b.send("Content-Length: " + std::to_string(body.size()) + "\r\n\r\nled=re");
        sleepMs(30);
        b.send("d&level=75");
        check(b.waitFor("{\"led\":\"red\",\"level\":75}"), "a POST form arriving in pieces");
        check(has(b.got(), "Transfer-Encoding: chunked\r\n"), "streamed chunked");

        b.send("GET /big HTTP/1.1\r\nHost: dev\r\n\r\n");
        check(b.waitFor("line 399 of a long streamed page\n\r\n0\r\n\r\n", 3000), "a 14 KB streamed page, whole");
        bool all = true;
        const std::string g = b.got();
        for (int i = 0; i < 400; i += 37) {
            char line[48];
            std::snprintf(line, sizeof line, "line %03d of a", i);
            all &= has(g, line);
        }
        check(all, "every line in it");

        b.send("POST /led HTTP/1.1\r\nHost: dev\r\nContent-Length: 5000\r\n\r\n");
        check(b.waitFor("413 Content Too Large") && b.closedByServer(), "a body over requestBytes: 413, closed");
        check(eventually([&] { return r.web->stats().active == 0; }), "and its thread ends");

        const xHttpServer::Stats st = r.web->stats();
        check(st.accepted == 1 && st.requests == 7 && st.errors == 2 && st.spawnFailures == 0, "stats");
    }

    std::printf("a thread per client\n");
    {
        Rig r;
        r.start();
        g_maxInSlow = 0;
        Browser a(r), b(r), c(r);
        check(a.connect() && b.connect() && c.connect(), "three browsers connect");
        check(eventually([&] { return r.web->stats().active == 3; }), "three client threads");
        check(r.listening() < 0, "every slot busy: nothing listens");
        const auto t0 = steady_clock::now();
        a.send("GET /slow HTTP/1.1\r\nHost: dev\r\n\r\n");
        b.send("GET /slow HTTP/1.1\r\nHost: dev\r\n\r\n");
        c.send(kGet);
        check(c.waitFor("hello", 200), "a fast request isn't held up behind slow ones");
        check(a.waitFor("slow", 1000) && b.waitFor("slow", 1000), "both slow requests answered");
        const long took = msSince(t0);
        check(g_maxInSlow == 2 && took < 550, "the slow handlers ran at the same time");

        a.close();
        check(eventually([&] { return r.web->stats().active == 2; }), "a browser closing ends its thread");
        check(eventually([&] { return r.listening() >= 0; }), "and frees a slot: the daemon listens again");
        Browser d(r);
        check(d.connect(), "a new browser takes it");
        d.send(kGet);
        check(d.waitFor("hello"), "and is answered");
    }

    std::printf("timeouts\n");
    {
        xHttpServer::Config cfg = Rig::defaults();
        cfg.idleTimeoutMs = 300;
        cfg.requestTimeoutMs = 400;
        Rig r(cfg);
        r.start();
        Browser a(r);
        a.connect();
        const auto t0 = steady_clock::now();
        check(a.closedByServer(1500), "an idle connection is closed");
        const long idle = msSince(t0);
        check(idle >= 250 && idle < 900, "after idleTimeoutMs");
        check(a.got().empty(), "without a response");

        Browser b(r);
        b.connect();
        b.send(kGet);
        check(b.waitFor("hello"), "a request answered");
        const auto t1 = steady_clock::now();
        b.send("GET / HT");
        check(b.waitFor("408 Request Timeout", 1500) && b.closedByServer(), "a request that stalls: 408, closed");
        const long slowReq = msSince(t1);
        check(slowReq >= 350 && slowReq < 1000, "after requestTimeoutMs");
        check(eventually([&] { return r.web->stats().timeouts == 2 && r.web->stats().active == 0; }), "counted");
    }

    std::printf("no thread to be had\n");
    {
        Rig r;
        r.start();
        simrtos::failTaskCreate() = 1;
        Browser a(r);
        a.connect();
        check(a.waitFor("503 Service Unavailable") && a.closedByServer(), "503, closed");
        check(eventually([&] { return r.web->stats().spawnFailures == 1; }), "counted");
        Browser b(r);
        check(b.connect(), "the next browser connects");
        b.send(kGet);
        check(b.waitFor("hello"), "and is served");
    }

    std::printf("stop\n");
    {
        Rig r;
        r.start();
        const int running = simrtos::tasksRunning();
        Browser a(r), b(r);
        a.connect();
        b.connect();
        a.send(kGet);
        check(a.waitFor("hello"), "two connections, one used");
        check(eventually([&] { return simrtos::tasksRunning() == running + 2; }), "two client threads");
        const auto t0 = steady_clock::now();
        r.stopServer();
        check(msSince(t0) < 1500, "stop(): run() returns promptly");
        check(r.web->stats().active == 0 && a.closedByServer(100) && b.closedByServer(100),
              "after closing both connections");
        check(eventually([&] { return simrtos::tasksRunning() == running; }), "and both threads have ended");
        check(r.listening() < 0, "nothing listens");
    }

    std::printf("arguments\n");
    {
        xHttpServer::Config cfg = Rig::defaults();
        cfg.maxClients = 0;
        Rig r(cfg);
        check(!r.web->begin(), "begin() refuses maxClients 0");
    }

    std::printf("%s\n", g_failures ? "FAILED" : "all passed");
    return g_failures ? 1 : 0;
}
