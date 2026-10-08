// Host test for SocketNetDevice over lwIP itself: lwIP 2.2.1 built for
// the PC (its Unix port: threads and semaphores from pthreads) with only
// its loopback netif. Two interfaces on that one stack, a server and a
// client, each an xEthernet on a SocketNetDevice built with
// INET_SOCKETS_LWIP and lwipNetifPlatform(), so every byte goes through
// lwIP's TCP, as it would on an STM32F207 or an ESP32: xHttpServer to
// xHttpClient, xClient to an lwIP echo server, DNS and SNTP over lwIP UDP.
//
// Built by CMake when LWIP_DIR points at an lwIP source tree.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include "lwip/tcpip.h"
#include "lwip/netif.h"
#include "lwip/sockets.h"
#include "LwipNetif.h"
#include "SocketNetDevice.h"
#include "SimDnsServer.h"
#include "SimNtpServer.h"
#include "xEthernet.h"
#include "xClient.h"
#include "xHttpClient.h"
#include "xHttpServer.h"
#include "HttpFiles.h"

using namespace std::chrono;

static int g_failures = 0;
static void check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failures;
}
static long msSince(steady_clock::time_point t0) {
    return static_cast<long>(duration_cast<milliseconds>(steady_clock::now() - t0).count());
}

// An interface on lwIP's loopback netif, with its driver thread.
struct Iface {
    SocketNetDevice dev;
    xEthernet eth;
    std::atomic<bool> quit{false};
    std::thread driver;
    static SocketNetDevice::Config devConfig(struct netif* lo) {
        SocketNetDevice::Config c;
        c.platform = lwipNetifPlatform(lo);
        return c;
    }
    static xNetInterface::Config netConfig() {
        xNetInterface::Config c;
        c.maxSockets = 8;
        c.ntpServer = "127.0.0.1";
        c.ntpIntervalMs = 0;
        return c;
    }
    explicit Iface(struct netif* lo) : dev(devConfig(lo)), eth(dev, netConfig()) {
        NetConfig net;
        net.mac = MacAddress(0x02, 0, 0, 0, 0, 2);
        net.dns = IpAddress(127, 0, 0, 1);   // lwIP's DNS is off here: the device's DnsClient asks this
        eth.begin(net);
        driver = std::thread([this] { while (!quit) eth.service(20); });
    }
    ~Iface() {
        quit = true;
        driver.join();
    }
};

// A UDP server on lwIP's 127.0.0.1:port.
struct LwipUdpServer {
    int fd = -1;
    std::atomic<bool> quit{false};
    std::thread t;
    template <typename F>
    bool start(uint16_t port, F handle) {
        fd = lwip_socket(AF_INET, SOCK_DGRAM, 0);
        sockaddr_in a;
        std::memset(&a, 0, sizeof a);
        a.sin_family = AF_INET;
        a.sin_port = lwip_htons(port);
        a.sin_addr.s_addr = PP_HTONL(INADDR_LOOPBACK);
        if (lwip_bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) return false;
        t = std::thread([this, handle] {
            uint8_t buf[600];
            while (!quit) {
                fd_set rd;
                FD_ZERO(&rd);
                FD_SET(fd, &rd);
                timeval tv{0, 50000};
                if (lwip_select(fd + 1, &rd, nullptr, nullptr, &tv) <= 0) continue;
                sockaddr_in from;
                socklen_t len = sizeof from;
                const int n = lwip_recvfrom(fd, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &len);
                if (n <= 0) continue;
                const std::vector<uint8_t> reply = handle(buf, static_cast<size_t>(n));
                if (!reply.empty()) lwip_sendto(fd, reply.data(), reply.size(), 0, reinterpret_cast<sockaddr*>(&from), len);
            }
        });
        return true;
    }
    ~LwipUdpServer() {
        quit = true;
        if (t.joinable()) t.join();
        if (fd >= 0) lwip_close(fd);
    }
};

static void hello(const HttpRequest&, HttpResponse& res, void*) { res.send("text/plain", "hello over lwIP"); }
static void slow(const HttpRequest&, HttpResponse& res, void*) {
    std::this_thread::sleep_for(milliseconds(300));
    res.send("text/plain", "slow");
}
static void big(const HttpRequest&, HttpResponse& res, void*) {
    res.begin("text/plain");
    for (int i = 0; i < 2000; ++i) res.printf("line %04d of a page streamed through lwIP\n", i);
}

int main() {
    std::atomic<bool> up{false};
    tcpip_init([](void* a) { static_cast<std::atomic<bool>*>(a)->store(true); }, &up);
    while (!up) std::this_thread::sleep_for(milliseconds(1));
    LOCK_TCPIP_CORE();
    struct netif* lo = netif_find("lo0");
    UNLOCK_TCPIP_CORE();

    std::printf("lwIP\n");
    check(lo != nullptr, "lwIP up, with its loopback netif");
    if (!lo) return 1;
    Iface server(lo), client(lo);
    check(server.eth.waitAddress(2000) && client.eth.waitAddress(2000), "both interfaces have an address");
    check(server.eth.address().ip == IpAddress(127, 0, 0, 1) && server.eth.address().subnet == IpAddress(255, 0, 0, 0),
          "lwIP's: read from the netif by lwipNetifPlatform()");

    static const char kPage[] = "<h1>page from flash</h1>";
    const HttpMemoryFiles::File files[] = {{"/index.html", reinterpret_cast<const uint8_t*>(kPage), sizeof kPage - 1}};
    HttpMemoryFiles mem(files, 1);
    HttpStaticFiles site(mem);
    xHttpServer::Config wcfg;
    wcfg.maxClients = 3;
    xHttpServer web(server.eth, wcfg);
    web.get("/hello", hello);
    web.get("/slow", slow);
    web.get("/big", big);
    web.get("/*", HttpStaticFiles::handler, &site);
    web.begin();
    std::thread daemon([&] { web.run(); });

    std::printf("xHttpServer to xHttpClient, through lwIP's TCP\n");
    {
        xHttpClient http(client.eth);
        uint8_t buf[256];
        xHttpClient::Response res;
        uint16_t st = 0;
        for (int i = 0; i < 50 && (st = http.get("http://127.0.0.1/hello", buf, sizeof buf, res, 2000)) != 200; ++i) {
            std::this_thread::sleep_for(milliseconds(20));
        }
        check(st == 200 && std::strcmp(reinterpret_cast<char*>(buf), "hello over lwIP") == 0, "GET");
        check(http.get("http://127.0.0.1/", buf, sizeof buf, res, 2000) == 200 &&
                  std::strcmp(reinterpret_cast<char*>(buf), kPage) == 0,
              "a page from files");
        std::string page;
        xHttpClient::Request q;
        q.url = "http://127.0.0.1/big";
        q.onBody = [](const uint8_t* d, size_t n, void* ctx) { static_cast<std::string*>(ctx)->append(reinterpret_cast<const char*>(d), n); return true; };
        q.ctx = &page;
        check(http.request(q, res, 5000) == 200 && page.size() == 2000 * 42 && page.find("line 1999 of a") != std::string::npos,
              "an 84 KB streamed page, chunked");
        check(web.stats().accepted == 1, "all on one kept connection");
        http.close();   // or it holds one of the server's three client slots
        const auto tc = steady_clock::now();
        while (web.stats().active != 0 && msSince(tc) < 1000) std::this_thread::sleep_for(milliseconds(2));
        check(web.stats().active == 0, "closed: the server's client thread ends");

        std::atomic<int> ok{0};
        const auto t0 = steady_clock::now();
        std::vector<std::thread> ts;
        for (int i = 0; i < 3; ++i) {
            ts.emplace_back([&] {
                xHttpClient h(client.eth);
                uint8_t b[32];
                xHttpClient::Response r;
                if (h.get("http://127.0.0.1/slow", b, sizeof b, r, 3000) == 200) ++ok;
            });
        }
        for (auto& t : ts) t.join();
        const long took = msSince(t0);
        check(ok == 3 && took < 500, "three slow requests on three client threads at once");
        std::printf("  (took %ld ms)\n", took);
    }

    std::printf("xClient to an lwIP echo server\n");
    {
        const int ls = lwip_socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a;
        std::memset(&a, 0, sizeof a);
        a.sin_family = AF_INET;
        a.sin_port = lwip_htons(7);
        a.sin_addr.s_addr = PP_HTONL(INADDR_LOOPBACK);
        lwip_bind(ls, reinterpret_cast<sockaddr*>(&a), sizeof a);
        lwip_listen(ls, 2);
        std::thread srv([&] {
            const int c = lwip_accept(ls, nullptr, nullptr);
            char buf[4096];
            int n;
            while ((n = lwip_recv(c, buf, sizeof buf, 0)) > 0) lwip_send(c, buf, static_cast<size_t>(n), 0);
            lwip_close(c);
        });
        xClient c(client.eth);
        check(c.connect(IpAddress(127, 0, 0, 1), 7, 2000), "connect");
        const size_t total = 256 * 1024;
        std::vector<uint8_t> out(total), in;
        for (size_t i = 0; i < total; ++i) out[i] = static_cast<uint8_t>(i * 13 + (i >> 9));
        const auto t0 = steady_clock::now();
        std::thread writer([&] {
            size_t at = 0;
            while (at < total) {
                const int32_t w = c.write(out.data() + at, std::min<size_t>(2048, total - at), 2000);
                if (w <= 0) break;
                at += static_cast<size_t>(w);
            }
        });
        uint8_t buf[2048];
        while (in.size() < total) {
            const int32_t n = c.read(buf, sizeof buf, 3000);
            if (n <= 0) break;
            in.insert(in.end(), buf, buf + n);
        }
        writer.join();
        check(in == out, "256 KB echoed, every byte");
        std::printf("  (in %ld ms)\n", msSince(t0));
        c.stop();
        srv.join();
        lwip_close(ls);
        xClient c2(client.eth);
        check(!c2.connect(IpAddress(127, 0, 0, 1), 9, 2000), "nothing listening: refused");
    }

    std::printf("DNS and SNTP, over lwIP UDP\n");
    {
        SimDnsServer dns;
        dns.a["plc.local"] = IpAddress(192, 168, 7, 7);
        SimNtpServer ntp;
        LwipUdpServer d, n;
        check(d.start(53, [&](const uint8_t* p, size_t len) { return dns.handle(p, len); }) &&
                  n.start(123, [&](const uint8_t* p, size_t len) { return ntp.handle(p, len); }),
              "servers on lwIP's 127.0.0.1:53 and :123");
        IpAddress ip;
        check(client.eth.resolve("plc.local", ip, 2000) && ip == IpAddress(192, 168, 7, 7), "resolve()");
        check(client.eth.syncTime(2000), "syncTime()");
        const int64_t diff = static_cast<int64_t>(client.eth.unixTimeMs()) - static_cast<int64_t>(ntp.unixMs);
        check(diff >= 0 && diff < 1000, "the server's time");
    }

    std::printf("the link going down\n");
    {
        xClient c(client.eth);
        LOCK_TCPIP_CORE();
        netif_set_link_down(lo);
        UNLOCK_TCPIP_CORE();
        const auto t0 = steady_clock::now();
        bool lost = false;
        while (msSince(t0) < 1000 && !lost) {
            lost = client.eth.address().ip == IpAddress();
            std::this_thread::sleep_for(milliseconds(5));
        }
        check(lost, "the interface loses its address");
        LOCK_TCPIP_CORE();
        netif_set_link_up(lo);
        UNLOCK_TCPIP_CORE();
        check(client.eth.waitAddress(1000), "and gets it back with the link");
    }

    web.stop();
    daemon.join();
    std::printf("%s\n", g_failures ? "FAILED" : "all passed");
    return g_failures ? 1 : 0;
}
