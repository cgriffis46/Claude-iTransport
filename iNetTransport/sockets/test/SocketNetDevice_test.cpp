// Host test for SocketNetDevice on the operating system's sockets: the
// whole stack (xEthernet, xClient, xHttpServer with files, xHttpClient)
// over real TCP on the loopback interface, driven by curl, with DNS and
// SNTP answered by the simulated servers bound to 127.0.0.3:53 and
// 127.0.0.4:123 (so it needs to run as root, or skips those). The same
// device, built with INET_SOCKETS_LWIP, is what runs on an STM32F207 or
// an ESP32; Lwip_test runs it over lwIP itself.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <vector>
#include "NetSockets.h"
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
static bool has(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

// A shell command's output.
static std::string run(const std::string& cmd, int* status = nullptr) {
    std::string out;
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return out;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, p)) > 0) out.append(buf, n);
    const int st = pclose(p);
    if (status) *status = st;
    return out;
}

// A port nothing is using now.
static uint16_t freePort() {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a);
    socklen_t len = sizeof a;
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len);
    ::close(fd);
    return ntohs(a.sin_port);
}

// A UDP server on addr:port answering with handle(); false if it can't bind.
struct UdpServer {
    int fd = -1;
    std::atomic<bool> quit{false};
    std::thread t;
    template <typename F>
    bool start(const char* addr, uint16_t port, F handle) {
        fd = ::socket(AF_INET, SOCK_DGRAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        inet_pton(AF_INET, addr, &a.sin_addr);
        if (::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) { ::close(fd); fd = -1; return false; }
        timeval tv{0, 50000};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        t = std::thread([this, handle] {
            uint8_t buf[1500];
            while (!quit) {
                sockaddr_in from{};
                socklen_t len = sizeof from;
                const long n = ::recvfrom(fd, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &len);
                if (n <= 0) continue;
                const std::vector<uint8_t> reply = handle(buf, static_cast<size_t>(n));
                if (!reply.empty()) ::sendto(fd, reply.data(), reply.size(), 0, reinterpret_cast<sockaddr*>(&from), len);
            }
        });
        return true;
    }
    ~UdpServer() {
        quit = true;
        if (t.joinable()) t.join();
        if (fd >= 0) ::close(fd);
    }
};

static void hello(const HttpRequest&, HttpResponse& res, void*) { res.send("text/plain", "hello"); }
static void slow(const HttpRequest&, HttpResponse& res, void*) {
    std::this_thread::sleep_for(milliseconds(300));
    res.send("text/plain", "slow");
}
static void echo(const HttpRequest& req, HttpResponse& res, void*) {
    res.send("application/octet-stream", req.body(), req.bodyLength());
}

int main() {
    // ---- the interface ----
    SocketNetDevice dev;
    xNetInterface::Config ncfg;
    ncfg.maxSockets = 8;
    ncfg.ntpServer = "127.0.0.4";
    ncfg.ntpIntervalMs = 0;
    xEthernet eth(dev, ncfg);
    NetConfig net;
    net.mac = MacAddress(0x02, 0, 0, 0, 0, 1);
    net.ip = IpAddress(127, 0, 0, 1);
    net.subnet = IpAddress(255, 0, 0, 0);
    net.dns = IpAddress(127, 0, 0, 3);
    eth.begin(net);
    std::atomic<bool> quit{false};
    std::thread driver([&] { while (!quit) eth.service(20); });

    std::printf("bring-up\n");
    check(eth.waitAddress(2000), "an address");
    check(eth.address().ip == IpAddress(127, 0, 0, 1), "the configured one");

    // ---- the web server, with files from a folder ----
    char dir[] = "/tmp/webrootXXXXXX";
    if (mkdtemp(dir) == nullptr) return 1;
    const std::string root = dir;
    FILE* f = std::fopen((root + "/index.html").c_str(), "w");
    std::fputs("<h1>user's page</h1>", f);
    std::fclose(f);
    run("printf 'compressed body' | gzip -c > " + root + "/app.js.gz");
    HttpStdioFiles files(dir);
    HttpStaticFiles site(files);

    const uint16_t port = freePort();
    xHttpServer::Config wcfg;
    wcfg.port = port;
    wcfg.maxClients = 3;
    xHttpServer web(eth, wcfg);
    web.get("/hello", hello);
    web.get("/slow", slow);
    web.post("/echo", echo);
    web.get("/*", HttpStaticFiles::handler, &site);
    check(web.begin(), "web server begin()");
    std::thread daemon([&] { web.run(); });
    const std::string base = "http://127.0.0.1:" + std::to_string(port);

    std::printf("the web server, to curl\n");
    {
        std::string out;
        for (int i = 0; i < 100 && (out = run("curl -s --max-time 2 " + base + "/hello")) != "hello"; ++i) {
            std::this_thread::sleep_for(milliseconds(20));
        }
        check(out == "hello", "curl GET /hello");
        check(run("curl -s --max-time 2 " + base + "/") == "<h1>user's page</h1>", "/ : index.html from the folder");
        check(run("curl -s --compressed --max-time 2 " + base + "/app.js") == "compressed body", "a gzipped file, unpacked by curl");
        const std::string head = run("curl -sI --max-time 2 " + base + "/");
        check(has(head, "HTTP/1.1 200 OK") && has(head, "Content-Length: 20"), "HEAD");
        check(run("curl -s --max-time 2 --data-binary 'posted data' " + base + "/echo") == "posted data", "POST");
        check(has(run("curl -s -o /dev/null -w '%{http_code}' --max-time 2 " + base + "/missing"), "404"), "404");

        const uint32_t acceptedBefore = web.stats().accepted;
        const std::string two = run("curl -s --max-time 2 " + base + "/hello " + base + "/hello");
        check(two == "hellohello" && web.stats().accepted == acceptedBefore + 1, "keep-alive: two requests, one connection");

        // Three slow requests on three client threads, at once; a fourth
        // waits in the listening socket's backlog rather than being refused.
        const auto t0 = steady_clock::now();
        const std::string par = run("for i in 1 2 3; do curl -s --max-time 3 " + base +
                                    "/slow & done; sleep 0.05; curl -s --max-time 3 " + base + "/hello; wait");
        const long took = msSince(t0);
        check(par.size() == 17 && has(par, "hello"), "three slow and one more, all answered");
        check(took < 800, "the slow ones in parallel (not 900+ ms), the fourth queued, not refused");
        char msg[64];
        std::snprintf(msg, sizeof msg, "  (took %ld ms)", took);
        std::printf("%s\n", msg);
    }

    std::printf("xHttpClient, over the same interface\n");
    {
        xHttpClient http(eth);
        uint8_t buf[256];
        xHttpClient::Response res;
        check(http.get((base + "/hello").c_str(), buf, sizeof buf, res, 3000) == 200 &&
                  std::strcmp(reinterpret_cast<char*>(buf), "hello") == 0,
              "GET to our own server");
        std::string big(20000, 0);
        for (size_t i = 0; i < big.size(); ++i) big[i] = static_cast<char>('a' + i % 26);
        std::string back;
        xHttpClient::Request q;
        const std::string echoUrl = base + "/echo";
        q.method = HttpMethod::Post;
        q.url = echoUrl.c_str();
        q.body = big.data();
        q.bodyLength = 900;   // within the server's requestBytes
        q.onBody = [](const uint8_t* d, size_t n, void* ctx) { static_cast<std::string*>(ctx)->append(reinterpret_cast<const char*>(d), n); return true; };
        q.ctx = &back;
        check(http.request(q, res, 3000) == 200 && back == big.substr(0, 900), "POST echoed");
    }

    std::printf("xClient, to an ordinary TCP server\n");
    {
        // An echo server on the host's own sockets.
        const uint16_t ep = freePort();
        const int ls = ::socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(ep);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ::bind(ls, reinterpret_cast<sockaddr*>(&a), sizeof a);
        ::listen(ls, 4);
        std::thread srv([&] {
            const int c = ::accept(ls, nullptr, nullptr);
            char buf[8192];
            long n;
            while ((n = ::recv(c, buf, sizeof buf, 0)) > 0) ::send(c, buf, static_cast<size_t>(n), MSG_NOSIGNAL);
            ::close(c);
            const int c2 = ::accept(ls, nullptr, nullptr);   // the second: closed at once
            ::close(c2);
        });

        xClient c(eth);
        check(c.connect(IpAddress(127, 0, 0, 1), ep, 2000), "connect");
        const size_t total = 1 << 20;
        std::vector<uint8_t> out(total), in;
        for (size_t i = 0; i < total; ++i) out[i] = static_cast<uint8_t>(i * 7 + (i >> 10));
        const auto t0 = steady_clock::now();
        std::thread writer([&] {
            size_t at = 0;
            while (at < total) {
                const int32_t w = c.write(out.data() + at, std::min<size_t>(4096, total - at), 2000);
                if (w <= 0) break;
                at += static_cast<size_t>(w);
            }
        });
        uint8_t buf[4096];
        while (in.size() < total) {
            const int32_t n = c.read(buf, sizeof buf, 3000);
            if (n <= 0) break;
            in.insert(in.end(), buf, buf + n);
        }
        writer.join();
        const long ms = msSince(t0);
        check(in == out, "1 MB echoed, every byte");
        std::printf("  (1 MB each way in %ld ms)\n", ms);
        c.stop();

        xClient c2(eth);
        check(c2.connect(IpAddress(127, 0, 0, 1), ep, 2000), "connect again");
        check(c2.read(buf, sizeof buf, 2000) == -1, "the server closing: read() returns -1");
        c2.stop();
        srv.join();
        ::close(ls);

        xClient c3(eth);
        const auto t1 = steady_clock::now();
        check(!c3.connect(IpAddress(127, 0, 0, 1), freePort(), 2000) && msSince(t1) < 500, "nothing listening: refused, at once");
    }

    std::printf("DNS and SNTP, on the device's UDP socket\n");
    {
        SimDnsServer dns;
        dns.a["sensors.local"] = IpAddress(10, 1, 2, 3);
        SimNtpServer ntp;
        UdpServer dnsSrv, ntpSrv;
        const bool canBind = dnsSrv.start("127.0.0.3", 53, [&](const uint8_t* p, size_t n) { return dns.handle(p, n); }) &&
                             ntpSrv.start("127.0.0.4", 123, [&](const uint8_t* p, size_t n) { return ntp.handle(p, n); });
        if (!canBind) {
            std::printf("  skip  (ports 53 and 123 need root)\n");
        } else {
            IpAddress ip;
            check(eth.resolve("sensors.local", ip, 2000) && ip == IpAddress(10, 1, 2, 3), "resolve() through DnsClient");
            check(!eth.resolve("nowhere.local", ip, 2000), "an unknown name");
            check(eth.syncTime(2000) && eth.timeValid(), "syncTime() through SntpClient");
            const int64_t diff = static_cast<int64_t>(eth.unixTimeMs()) - static_cast<int64_t>(ntp.unixMs);
            check(diff >= 0 && diff < 1000, "the server's time");
        }
    }

    web.stop();
    daemon.join();
    check(web.stats().active == 0, "web server stopped");
    quit = true;
    driver.join();
    run("rm -rf " + root);
    std::printf("%s\n", g_failures ? "FAILED" : "all passed");
    return g_failures ? 1 : 0;
}
