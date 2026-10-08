// Host test for xEthernet / xWifi / xClient: the FreeRTOS layer, run
// for real on std::threads against the FreeRTOS simulation in stub/,
// with the W5500 driver underneath talking to the simulated chip in
// sim/. A driver thread runs eth.service() exactly as eth.run() would
// on the target; the test's own thread is the user thread, and sleeps
// in client.read() etc. for real.
//
//   g++ -std=c++17 -Wall -Wextra -pthread -Istub -Isim -I../inc -I../hw/freertos/inc
//       -I../w5500/inc -I<iTransport>/itransport/inc xNet_test.cpp
//       ../hw/freertos/src/xNetInterface.cpp ../hw/freertos/src/xClient.cpp
//       ../hw/freertos/src/xWifi.cpp -o xNet_test
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include "xEthernet.h"
#include "xWifi.h"
#include "xClient.h"
#include "W5500.h"
#include "SimW5500.h"
#include "SimDhcpServer.h"
#include "SimDnsServer.h"
#include "SimNtpServer.h"
#include "EspAt.h"
#include "SimEspAt.h"

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
static bool eventually(F f, int ms = 1000) {
    const auto t0 = steady_clock::now();
    while (msSince(t0) < ms) {
        if (f()) return true;
        sleepMs(1);
    }
    return f();
}

static std::vector<uint8_t> pattern(size_t n, uint8_t seed) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = static_cast<uint8_t>(seed + i * 13 + (i >> 9));
    return v;
}

typedef W5500::w5500<FakeW5500Spi> Chip;

static const IpAddress kServer(192, 168, 1, 10);

// A W5500 behind an xEthernet, with its driver thread running.
struct Rig {
    std::unique_ptr<SimW5500> sim{new SimW5500};
    std::unique_ptr<Chip> chip;
    std::unique_ptr<xEthernet> eth;
    std::atomic<bool> quit{false};
    std::thread driver;

    SimDhcpServer dhcpSrv;
    SimDnsServer dnsSrv;
    SimNtpServer ntpSrv;

    explicit Rig(W5500::w5500_param_t p = W5500::w5500_param_t(),
                 xNetInterface::Config c = xNetInterface::Config(), bool irq = false, bool dhcp = false) {
        chip.reset(new Chip(p, *sim));
        eth.reset(new xEthernet(*chip, c));
        if (irq) sim->onIrq = [this] { eth->interruptFromIsr(); };
        NetConfig net;
        net.mac = MacAddress(0x02, 0, 0, 0x12, 0x34, 0x56);
        net.ip = IpAddress(192, 168, 1, 50);
        net.subnet = IpAddress(255, 255, 255, 0);
        net.gateway = IpAddress(192, 168, 1, 1);
        net.dhcp = dhcp;
        net.dns = dnsSrv.server;
        dhcpSrv.dns = dnsSrv.server;
        dhcpSrv.ntp = IpAddress();                     // DHCP names no NTP server unless a test says so
        ntpSrv.server = IpAddress(162, 159, 200, 1);   // where pool.ntp.org resolves to
        sim->onUdpSend = [this](uint8_t s, IpAddress dst, uint16_t port, std::vector<uint8_t> data) {
            std::vector<uint8_t> reply;
            IpAddress from = dst;
            if (port == 67) { reply = dhcpSrv.handle(data.data(), data.size()); from = dhcpSrv.server; }
            if (port == 53 && dst == dnsSrv.server) reply = dnsSrv.handle(data.data(), data.size());
            if (port == 123 && dst == ntpSrv.server) reply = ntpSrv.handle(data.data(), data.size());
            if (!reply.empty()) sim->peerSendUdp(s, from, port, reply.data(), reply.size());
        };
        eth->begin(net);
        driver = std::thread([this] { while (!quit) eth->service(20); });
    }
    ~Rig() {
        quit = true;
        driver.join();
        sim->onIrq = nullptr;
    }
};

// ---- a Wi-Fi module that does nothing but join ----
struct FakeWifi : iWifiDevice {
    iNetDeviceHost* host = nullptr;
    std::string ssid, pass;
    int pendingJoin = 0; // 1: join asked, answer on the next poll
    bool joined = false;

    void attach(iNetDeviceHost& h) override { host = &h; }
    uint8_t socketCount() const override { return 4; }
    void configure(const NetConfig&) override { host->deviceEvent(DeviceEvent::Ready); }
    uint32_t poll(uint32_t) override {
        if (pendingJoin) {
            pendingJoin = 0;
            joined = (ssid == "home" && pass == "secret");
            host->deviceEvent(joined ? DeviceEvent::LinkUp : DeviceEvent::JoinFailed);
        }
        return 50;
    }
    bool connect(uint8_t, const IpAddress&, uint16_t, uint16_t) override { return false; }
    bool listen(uint8_t, uint16_t) override { return false; }
    void close(uint8_t s) override { host->socketEvent(s, SocketEvent::Closed); }
    void interrupt() override {}
    bool resolve(const char*) override { return false; }
    bool requestTime(const char*) override { return false; }
    void join(const char* s, const char* p) override { ssid = s; pass = p; pendingJoin = 1; }
    void leave() override { joined = false; host->deviceEvent(DeviceEvent::LinkDown); }
    int8_t rssi() const override { return joined ? -55 : 0; }
};

int main() {
    std::printf("bring-up\n");
    {
        Rig r;
        check(r.eth->waitReady(1000), "waitReady()");
        check(r.eth->waitLinkUp(1000) && r.eth->linkUp(), "waitLinkUp()");
        check(r.eth->socketCount() == 4, "4 sockets (Config::maxSockets)");
        check(r.eth->waitAddress(1000) && r.eth->address().ip == IpAddress(192, 168, 1, 50), "static address in use");
    }

    std::printf("DHCP\n");
    {
        Rig r(W5500::w5500_param_t(), xNetInterface::Config(), false, /*dhcp=*/true);
        r.eth->waitReady(1000);
        xClient c(*r.eth);
        // Asked before there is an address: held until the lease arrives.
        const auto t0 = steady_clock::now();
        check(c.connect(kServer, 80, 2000), "connect() straight after begin() succeeds once leased");
        check(msSince(t0) < 1000, "promptly");
        check(r.eth->hasAddress(), "hasAddress()");
        const NetConfig a = r.eth->address();
        check(a.ip == r.dhcpSrv.offerIp && a.gateway == r.dhcpSrv.router && a.dns == r.dhcpSrv.dns, "address() is the lease");
    }

    std::printf("DNS through xEthernet\n");
    {
        Rig r;
        r.eth->waitAddress(1000);
        IpAddress ip;
        const int before = r.dnsSrv.queries;
        check(r.eth->resolve("10.1.2.3", ip, 0) && ip == IpAddress(10, 1, 2, 3) && r.dnsSrv.queries == before,
              "\"a.b.c.d\" answered at once, nothing asked");
        auto t0 = steady_clock::now();
        check(r.eth->resolve("pool.ntp.org", ip, 2000) && ip == IpAddress(162, 159, 200, 1), "resolve() sleeps for the answer");
        check(msSince(t0) < 500, "promptly");
        t0 = steady_clock::now();
        check(!r.eth->resolve("nosuch.example", ip, 2000) && msSince(t0) < 500, "unknown name: false, at once");

        // Two threads at once: they queue, and each gets its own answer.
        IpAddress a, b;
        bool okA = false, okB = false;
        std::thread ta([&] { okA = r.eth->resolve("www.example.com", a, 3000); });
        std::thread tb([&] { okB = r.eth->resolve("pool.ntp.org", b, 3000); });
        ta.join();
        tb.join();
        check(okA && okB && a == IpAddress(93, 184, 216, 34) && b == IpAddress(162, 159, 200, 1),
              "two threads resolving at once each get their own answer");

        r.dnsSrv.silent = true;
        t0 = steady_clock::now();
        check(!r.eth->resolve("pool.ntp.org", ip, 300), "no answer: false...");
        check(msSince(t0) >= 290 && msSince(t0) < 600, "...at the timeout");
        r.dnsSrv.silent = false;
        check(r.eth->resolve("www.example.com", ip, 3000) && ip == IpAddress(93, 184, 216, 34),
              "and the next lookup isn't confused by the one given up on");
    }

    std::printf("time through xEthernet\n");
    {
        Rig r;
        check(eventually([&] { return r.eth->timeValid(); }, 2000), "time set by itself once there is an address");
        const auto check0 = steady_clock::now();
        const uint64_t t = r.eth->unixTimeMs();
        check(t >= r.ntpSrv.unixMs && t < r.ntpSrv.unixMs + 3000, "unixTimeMs(): the server's time, carried on by the tick");
        sleepMs(200);
        const uint64_t t2 = r.eth->unixTimeMs();
        const long d = static_cast<long>(t2 - t) - msSince(check0);
        check(d > -20 && d < 20, "and it runs at the tick's rate");
        const int before = r.ntpSrv.requests;
        check(r.eth->syncTime(2000) && r.ntpSrv.requests == before + 1, "syncTime() asks again, and sleeps for the answer");
        r.ntpSrv.kod = true;
        check(!r.eth->syncTime(2000) && r.eth->timeValid(), "a refusal: false, the time kept");
    }

    std::printf("time from DHCP's NTP server\n");
    {
        Rig r(W5500::w5500_param_t(), xNetInterface::Config(), false, /*dhcp=*/true);
        r.dhcpSrv.ntp = IpAddress(192, 168, 1, 123);
        r.ntpSrv.server = r.dhcpSrv.ntp;
        check(r.eth->waitAddress(3000) && r.eth->address().ntp == r.dhcpSrv.ntp, "address() carries option 42");
        check(eventually([&] { return r.eth->timeValid(); }, 3000) && r.ntpSrv.requests > 0,
              "time taken from it, not from pool.ntp.org");
    }

    std::printf("connect, write, read\n");
    {
        Rig r;
        r.eth->waitReady(1000);
        xClient c(*r.eth);
        uint8_t buf[64];
        check(c.read(buf, sizeof buf, 0) == -1, "read() before connect(): -1");
        check(c.connect(kServer, 80, 1000) && c.connected(), "connect()");
        check(r.sim->connectedTo(c.socket()) == kServer && r.sim->connectedPort(c.socket()) == 80, "to the right place");

        const char req[] = "GET / HTTP/1.0\r\n\r\n";
        check(c.write(reinterpret_cast<const uint8_t*>(req), sizeof req - 1, 1000) == static_cast<int32_t>(sizeof req - 1), "write() queues it all");
        const std::vector<uint8_t> want(req, req + sizeof req - 1);
        check(eventually([&] { return r.sim->sent(c.socket()) == want; }), "and it reaches the chip");

        auto t0 = steady_clock::now();
        check(c.read(buf, sizeof buf, 100) == 0, "read() with nothing to read: 0 at the timeout");
        check(msSince(t0) >= 90, "after sleeping the whole timeout");

        std::thread peer([&] { sleepMs(50); r.sim->peerSend(static_cast<uint8_t>(c.socket()), reinterpret_cast<const uint8_t*>("hello"), 5); });
        t0 = steady_clock::now();
        const int32_t n = c.read(buf, sizeof buf, 2000);
        const long took = msSince(t0);
        peer.join();
        check(n == 5 && std::memcmp(buf, "hello", 5) == 0, "read() sleeps until data arrives, and returns it");
        check(took >= 40 && took < 500, "woken when it arrived, not at the timeout");
        check(c.available() == 0, "nothing left over");
    }

    std::printf("bulk transfer through every buffer\n");
    {
        Rig r;
        r.eth->waitReady(1000);
        xClient c(*r.eth);
        c.connect(kServer, 80, 1000);
        const uint8_t s = static_cast<uint8_t>(c.socket());

        // 20 KB: ten times the chip's 2 KB socket buffer, twenty times
        // the 1 KB stream buffer, so the driver has to stall and be
        // kicked by the reader over and over.
        const std::vector<uint8_t> down = pattern(20000, 5);
        std::atomic<bool> peerDone{false};
        std::thread peer([&] {
            size_t sent = 0;
            while (sent < down.size()) {
                sent += r.sim->peerSend(s, down.data() + sent, down.size() - sent);
                sleepMs(1);
            }
            peerDone = true;
        });
        std::vector<uint8_t> got;
        uint8_t buf[300];
        const auto t0 = steady_clock::now();
        while (got.size() < down.size() && msSince(t0) < 5000) {
            const int32_t n = c.read(buf, sizeof buf, 500);
            if (n > 0) got.insert(got.end(), buf, buf + n);
        }
        peer.join();
        check(got == down, "20 KB received intact");

        const std::vector<uint8_t> up = pattern(20000, 77);
        check(c.write(up.data(), up.size(), 5000) == static_cast<int32_t>(up.size()), "20 KB written");
        check(c.flush(2000), "flush()");
        check(eventually([&] { return r.sim->sent(s).size() >= up.size(); }, 2000) && r.sim->sent(s) == up, "20 KB sent intact");
    }

    std::printf("the peer closes while a reader sleeps\n");
    {
        Rig r;
        r.eth->waitReady(1000);
        xClient c(*r.eth);
        c.connect(kServer, 80, 1000);
        const uint8_t s = static_cast<uint8_t>(c.socket());
        std::thread peer([&] {
            sleepMs(50);
            r.sim->peerSend(s, reinterpret_cast<const uint8_t*>("bye"), 3);
            r.sim->peerClose(s);
        });
        uint8_t buf[16];
        const auto t0 = steady_clock::now();
        int32_t n = c.read(buf, sizeof buf, 5000);
        check(n == 3 && std::memcmp(buf, "bye", 3) == 0, "data sent before the close is read first");
        n = c.read(buf, sizeof buf, 5000);
        const long took = msSince(t0);
        peer.join();
        check(n == -1, "then read() returns -1");
        check(took < 1000, "woken by the close, not the 5 s timeout");
        check(!c.connected(), "connected() false");
        check(c.write(buf, 1, 100) == -1, "write() refused");
        check(eventually([&] { return r.sim->status(s) == W5500::w5500_SOCK_CLOSED; }), "our side closed on the chip");
    }

    std::printf("stop, and connect again on the same client\n");
    {
        Rig r;
        r.eth->waitReady(1000);
        xClient c(*r.eth);
        c.connect(kServer, 80, 1000);
        const int s = c.socket();
        c.stop();
        check(c.socket() == -1 && !c.connected(), "stop() gives the socket back");
        check(r.sim->status(static_cast<uint8_t>(s)) == W5500::w5500_SOCK_CLOSED, "closed on the chip");
        check(c.connect(kServer, 81, 1000) && c.connected(), "connect() again");
        uint8_t buf[4];
        check(c.read(buf, sizeof buf, 50) == 0, "no stale close from the last connection");
    }

    std::printf("connect failures\n");
    {
        Rig r;
        r.eth->waitReady(1000);
        xClient c(*r.eth);
        r.sim->connectPolicy = SimW5500::Connect::Refuse;
        auto t0 = steady_clock::now();
        check(!c.connect(kServer, 80, 2000), "refused: false");
        check(msSince(t0) < 500, "at once, not at the timeout");

        r.sim->connectPolicy = SimW5500::Connect::Pending; // SYN never answered
        t0 = steady_clock::now();
        check(!c.connect(kServer, 80, 200), "no answer: false");
        const long took = msSince(t0);
        check(took >= 190 && took < 600, "at the timeout");
        const uint8_t s = static_cast<uint8_t>(c.socket());
        check(eventually([&] { return r.sim->status(s) == W5500::w5500_SOCK_CLOSED; }), "and the attempt is abandoned on the chip");
    }

    std::printf("socket limit\n");
    {
        xNetInterface::Config cfg;
        cfg.maxSockets = 2;
        Rig r(W5500::w5500_param_t(), cfg);
        r.eth->waitReady(1000);
        xClient a(*r.eth), b(*r.eth), c(*r.eth);
        check(a.connect(kServer, 80, 1000) && b.connect(kServer, 80, 1000), "two connect");
        check(!c.connect(kServer, 80, 1000) && c.socket() == -1, "a third finds no socket");
        a.stop();
        check(c.connect(kServer, 80, 1000), "until one is given back");
    }

    std::printf("listen / accept\n");
    {
        Rig r;
        r.eth->waitReady(1000);
        xClient server(*r.eth);
        check(server.listen(8080), "listen()");
        const uint8_t s = static_cast<uint8_t>(server.socket());
        check(r.sim->status(s) == W5500::w5500_SOCK_LISTEN, "chip listening");
        check(!server.accept(50), "accept() times out with no peer");
        std::thread peer([&] { sleepMs(50); r.sim->peerConnect(s); sleepMs(20); r.sim->peerSend(s, reinterpret_cast<const uint8_t*>("hi"), 2); });
        check(server.accept(1000), "accept() wakes when a peer connects");
        uint8_t buf[4];
        check(server.read(buf, sizeof buf, 1000) == 2, "and reads from it");
        peer.join();
    }

    std::printf("interrupt line instead of polling\n");
    {
        W5500::w5500_param_t p;
        p.pollMs = 2000; // polling as a backstop only
        Rig r(p, xNetInterface::Config(), /*irq=*/true);
        r.eth->waitReady(1000);
        xClient c(*r.eth);
        c.connect(kServer, 80, 1000);
        sleepMs(50);
        const uint8_t s = static_cast<uint8_t>(c.socket());
        std::thread peer([&] { sleepMs(30); r.sim->peerSend(s, reinterpret_cast<const uint8_t*>("x"), 1); });
        uint8_t buf[4];
        const auto t0 = steady_clock::now();
        const int32_t n = c.read(buf, sizeof buf, 3000);
        const long took = msSince(t0);
        peer.join();
        check(n == 1 && took < 200, "data read within ms of the interrupt, not at the 2 s poll");
    }

    std::printf("xWifi\n");
    {
        FakeWifi dev;
        xWifi wifi(dev);
        std::atomic<bool> quit{false};
        wifi.begin(NetConfig());
        std::thread driver([&] { while (!quit) wifi.service(20); });
        check(wifi.waitReady(1000), "ready");
        check(!wifi.join("home", "wrong", 1000) && !wifi.linkUp(), "wrong passphrase: join() false");
        check(wifi.join("home", "secret", 1000) && wifi.linkUp(), "right one: join() true, link up");
        std::string longSsid(40, 'x');
        check(!wifi.join(longSsid.c_str(), "secret", 100), "over-long SSID refused");
        wifi.leave(1000);
        check(!wifi.linkUp(), "leave(): link down");
        quit = true;
        driver.join();
    }

    std::printf("xWifi on an ESP-AT module\n");
    {
        SimEspAt sim;
        ESPAT::espat<FakeEspUart> esp(ESPAT::espat_param_t(), sim);
        xWifi wifi(esp);
        NetConfig net;
        net.dhcp = true;
        wifi.begin(net);
        std::atomic<bool> quit{false};
        std::thread driver([&] { while (!quit) wifi.service(50); });

        check(wifi.waitReady(2000), "module up");
        check(!wifi.join("home", "nope", 2000), "wrong passphrase: join() false");
        check(wifi.join("home", "secret", 2000) && wifi.linkUp(), "join() true");
        check(wifi.waitAddress(1000) && wifi.address().ip == sim.ip, "address from the module's DHCP");
        IpAddress ip;
        check(wifi.resolve("pool.ntp.org", ip, 3000) && ip == IpAddress(162, 159, 200, 1), "resolve() through AT+CIPDOMAIN");
        check(wifi.syncTime(8000) && wifi.timeValid(), "syncTime() through the module's SNTP");
        const uint64_t t = wifi.unixTimeMs();
        check(t >= uint64_t(sim.unixSec) * 1000 && t < uint64_t(sim.unixSec) * 1000 + 4000, "unixTimeMs()");
        {
            xClient c(wifi);
            check(c.connect(IpAddress(93, 184, 216, 34), 80, 2000), "connect()");
            const char req[] = "GET / HTTP/1.0\r\n\r\n";
            c.write(reinterpret_cast<const uint8_t*>(req), sizeof req - 1, 1000);
            check(eventually([&] { return sim.sent(0).size() == sizeof req - 1; }), "write() reaches the module");

            std::thread peer([&] { sleepMs(50); sim.peerSend(0, {'h', 'e', 'l', 'l', 'o'}); });
            uint8_t buf[512];
            const auto t0 = steady_clock::now();
            const int32_t n = c.read(buf, sizeof buf, 3000);
            const long took = msSince(t0);
            peer.join();
            check(n == 5 && std::memcmp(buf, "hello", 5) == 0 && took < 500,
                  "read() sleeps until the module announces data, then fetches it");

            // 8 KB down through a 1 KB stream buffer: fetched only as
            // the reader makes room.
            const std::vector<uint8_t> down = pattern(8000, 21);
            sim.peerSend(0, down);
            std::vector<uint8_t> got;
            const auto t1 = steady_clock::now();
            while (got.size() < down.size() && msSince(t1) < 5000) {
                const int32_t k = c.read(buf, 300, 500);
                if (k > 0) got.insert(got.end(), buf, buf + k);
            }
            check(got == down, "8 KB received intact");

            std::thread closer([&] { sleepMs(50); sim.peerClose(0); });
            const auto t2 = steady_clock::now();
            check(c.read(buf, sizeof buf, 5000) == -1 && msSince(t2) < 1000, "peer close wakes the reader: -1");
            closer.join();
        }
        quit = true;
        driver.join();
    }

    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "PASSED", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
