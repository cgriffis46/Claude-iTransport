// Host test for xHttpClient: the calling thread and the W5500's driver
// thread run for real on std::threads against the FreeRTOS simulation in
// stub/. The simulated chip hands the client's TCP connections to a
// simulated HTTP server below, which answers through the chip from a
// thread of its own (so big responses can trickle through the chip's
// receive buffer); the server's name is looked up through SimDnsServer.
//
//   g++ -std=c++17 -Wall -Wextra -pthread -Istub -Isim -I../inc -I../hw/freertos/inc
//       -I../w5500/inc -I../http/inc ... xHttpClient_test.cpp ../hw/freertos/src/*.cpp
//       ../w5500/src/*.cpp ../http/src/*.cpp ... -o xHttpClient_test
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include "xEthernet.h"
#include "xHttpClient.h"
#include "W5500.h"
#include "SimW5500.h"
#include "SimDnsServer.h"

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
static bool has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

static std::string pattern(size_t n) {
    std::string s(n, 0);
    for (size_t i = 0; i < n; ++i) s[i] = static_cast<char>('a' + (i * 7 + i / 251) % 26);
    return s;
}

typedef W5500::w5500<FakeW5500Spi> Chip;
static const IpAddress kServer(192, 168, 1, 20);

// An HTTP server at the far end of the chip's TCP connections.
class SimHttpServer {
public:
    struct Req { int s; std::string method, path, head, body; };

    explicit SimHttpServer(SimW5500& sim) : sim_(sim), sender_([this] { send(); }) {}
    ~SimHttpServer() {
        { std::lock_guard<std::mutex> g(m_); quit_ = true; }
        cv_.notify_all();
        sender_.join();
    }

    void onConnect(uint8_t s) {
        std::lock_guard<std::mutex> g(m_);
        in_[s].clear();
        open_.insert(s);
        ++gen_[s];
        ++connects_;
    }

    void onData(uint8_t s, const std::vector<uint8_t>& d) {
        std::lock_guard<std::mutex> g(m_);
        std::string& in = in_[s];
        in.append(d.begin(), d.end());
        for (;;) {
            const size_t end = in.find("\r\n\r\n");
            if (end == std::string::npos) return;
            Req r;
            r.s = s;
            r.head = in.substr(0, end + 4);
            const size_t sp1 = r.head.find(' '), sp2 = r.head.find(' ', sp1 + 1);
            r.method = r.head.substr(0, sp1);
            r.path = r.head.substr(sp1 + 1, sp2 - sp1 - 1);
            size_t len = 0;
            const size_t cl = r.head.find("Content-Length: ");
            if (cl != std::string::npos) len = std::stoul(r.head.substr(cl + 16));
            if (in.size() < end + 4 + len) return;
            r.body = in.substr(end + 4, len);
            in.erase(0, end + 4 + len);
            reqs_.push_back(r);
            answer(r);
        }
    }

    // The server closes every open connection (an idle timeout, say).
    void closeAll() {
        std::lock_guard<std::mutex> g(m_);
        for (int s : open_) queue(s, "", true, 0);
        open_.clear();
    }

    int connects() { std::lock_guard<std::mutex> g(m_); return connects_; }
    std::vector<Req> requests() { std::lock_guard<std::mutex> g(m_); return reqs_; }

private:
    // gen: which connection on socket s it is for. A later connection on
    // the same socket doesn't get what was left for an earlier one.
    struct Out { int s; unsigned gen; std::string data; bool close; int delayMs; };

    void queue(int s, const std::string& d, bool close, int delayMs) {
        out_.push_back(Out{s, gen_[s], d, close, delayMs});
        cv_.notify_all();
    }

    void answer(const Req& r) {
        const int s = r.s;
        const std::string& p = r.path;
        if (p == "/hello") return queue(s, "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 5\r\n\r\nhello", false, 0);
        if (p == "/echo") {
            return queue(s, "HTTP/1.1 201 Created\r\nContent-Length: " + std::to_string(r.body.size()) + "\r\n\r\n" + r.body,
                         false, 0);
        }
        if (p == "/chunked") {
            queue(s, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n7\r\nHello, ", false, 0);
            queue(s, "\r\n8;x=y\r\nchunked \r\n", false, 30);
            return queue(s, "5\r\nworld\r\n0\r\n\r\n", false, 30);
        }
        if (p == "/close") { open_.erase(s); return queue(s, "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nbye now", true, 0); }
        if (p == "/http10") return queue(s, "HTTP/1.0 200 OK\r\nContent-Length: 2\r\n\r\nok", false, 0);
        if (p == "/big") {
            const std::string b = pattern(20000);
            return queue(s, "HTTP/1.1 200 OK\r\nContent-Length: 20000\r\n\r\n" + b, false, 0);
        }
        if (p == "/head") return queue(s, "HTTP/1.1 200 OK\r\nContent-Length: 1000\r\n\r\n", false, 0);
        if (p == "/continue") {
            queue(s, "HTTP/1.1 100 Continue\r\n\r\n", false, 0);
            return queue(s, "HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\ndone", false, 20);
        }
        if (p == "/silent") return;
        if (p == "/drop") { open_.erase(s); return queue(s, "", true, 0); }
        if (p == "/dropOnce") {
            if (droppedOnce_++ == 0) { open_.erase(s); return queue(s, "", true, 0); }
            return queue(s, "HTTP/1.1 200 OK\r\nContent-Length: 6\r\n\r\nsecond", false, 0);
        }
        if (p == "/garbage") return queue(s, "HTTP/1.1 OK\r\n\r\n", false, 0);
        queue(s, "HTTP/1.1 404 Not Found\r\nContent-Length: 9\r\n\r\nnot found", false, 0);
    }

    void send() {
        for (;;) {
            Out o;
            {
                std::unique_lock<std::mutex> l(m_);
                cv_.wait(l, [&] { return quit_ || !out_.empty(); });
                if (quit_) return;
                o = out_.front();
                out_.pop_front();
            }
            if (o.delayMs) sleepMs(o.delayMs);
            auto current = [&] {
                std::lock_guard<std::mutex> g(m_);
                return gen_[o.s] == o.gen &&
                       sim_.status(static_cast<uint8_t>(o.s)) == W5500::w5500_SOCK_ESTABLISHED;
            };
            size_t at = 0;
            while (at < o.data.size() && current()) {
                at += sim_.peerSend(static_cast<uint8_t>(o.s), reinterpret_cast<const uint8_t*>(o.data.data()) + at,
                                    o.data.size() - at);
                if (at < o.data.size()) sleepMs(1);
            }
            if (o.close && current()) sim_.peerClose(static_cast<uint8_t>(o.s));
        }
    }

    SimW5500& sim_;
    std::mutex m_;
    std::condition_variable cv_;
    std::map<int, std::string> in_;
    std::map<int, unsigned> gen_;
    std::set<int> open_;
    std::deque<Out> out_;
    std::vector<Req> reqs_;
    int connects_ = 0, droppedOnce_ = 0;
    bool quit_ = false;
    std::thread sender_;
};

// A W5500 behind an xEthernet with its driver thread, DNS, and the server.
struct Rig {
    std::unique_ptr<SimW5500> sim{new SimW5500};
    std::unique_ptr<SimHttpServer> srv{new SimHttpServer(*sim)};
    SimDnsServer dns;
    std::unique_ptr<Chip> chip;
    std::unique_ptr<xEthernet> eth;
    std::atomic<bool> quit{false};
    std::thread driver;

    explicit Rig(bool refuse = false) {
        if (refuse) sim->connectPolicy = SimW5500::Connect::Refuse;
        chip.reset(new Chip(W5500::w5500_param_t(), *sim));
        eth.reset(new xEthernet(*chip));
        NetConfig net;
        net.mac = MacAddress(0x02, 0, 0, 0x12, 0x34, 0x56);
        net.ip = IpAddress(192, 168, 1, 50);
        net.subnet = IpAddress(255, 255, 255, 0);
        net.gateway = IpAddress(192, 168, 1, 1);
        net.dhcp = false;
        net.dns = dns.server;
        dns.a["sensors.local"] = kServer;
        sim->onUdpSend = [this](uint8_t s, IpAddress dst, uint16_t port, std::vector<uint8_t> data) {
            if (port != 53 || dst != dns.server) return;
            const std::vector<uint8_t> reply = dns.handle(data.data(), data.size());
            if (!reply.empty()) sim->peerSendUdp(s, dst, port, reply.data(), reply.size());
        };
        sim->onTcpConnect = [this](uint8_t s, IpAddress dst, uint16_t) { if (dst == kServer) srv->onConnect(s); };
        sim->onTcpSend = [this](uint8_t s, std::vector<uint8_t> d) { srv->onData(s, d); };
        eth->begin(net);
        driver = std::thread([this] { while (!quit) eth->service(20); });
    }
    ~Rig() {
        quit = true;
        driver.join();
        sim->onTcpSend = nullptr;
        sim->onTcpConnect = nullptr;
        srv.reset();
    }
};

struct Streamed {
    std::string data;
    size_t stopAt = 0;
    static bool fn(const uint8_t* d, size_t n, void* ctx) {
        Streamed& s = *static_cast<Streamed*>(ctx);
        s.data.append(reinterpret_cast<const char*>(d), n);
        return s.stopAt == 0 || s.data.size() < s.stopAt;
    }
};

int main() {
    const char* base = "http://sensors.local:8080";
    auto url = [&](const char* path) { return std::string(base) + path; };

    std::printf("requests\n");
    {
        Rig r;
        xHttpClient http(*r.eth);
        uint8_t buf[256];
        xHttpClient::Response res;

        check(http.get(url("/hello").c_str(), buf, sizeof buf, res, 3000) == 200, "GET: 200");
        check(res.length == 5 && std::strcmp(reinterpret_cast<char*>(buf), "hello") == 0 && !res.truncated,
              "the body, NUL-terminated");
        auto reqs = r.srv->requests();
        check(reqs.size() == 1 && reqs[0].head == "GET /hello HTTP/1.1\r\nHost: sensors.local:8080\r\n"
                                                  "User-Agent: iNetTransport\r\n\r\n",
              "the request as sent: looked up by name, Host with the port");

        const char json[] = "{\"node\":3,\"t\":21.5}";
        check(http.post(url("/echo").c_str(), "application/json", json, sizeof json - 1, buf, sizeof buf, res, 3000) ==
                  201,
              "POST: 201");
        reqs = r.srv->requests();
        check(reqs.size() == 2 && reqs[1].body == json && has(reqs[1].head, "Content-Type: application/json\r\n") &&
                  has(reqs[1].head, "Content-Length: 19\r\n"),
              "the server got the JSON, typed and with its length");
        check(std::strcmp(reinterpret_cast<char*>(buf), json) == 0, "and echoed it");

        check(http.get(url("/chunked").c_str(), buf, sizeof buf, res, 3000) == 200 &&
                  std::strcmp(reinterpret_cast<char*>(buf), "Hello, chunked world") == 0,
              "a chunked body arriving in pieces, joined");
        check(http.get(url("/continue").c_str(), buf, sizeof buf, res, 3000) == 200 &&
                  std::strcmp(reinterpret_cast<char*>(buf), "done") == 0,
              "100 Continue passed over");
        xHttpClient::Request head;
        head.method = HttpMethod::Head;
        const std::string headUrl = url("/head");
        head.url = headUrl.c_str();
        check(http.request(head, res, 3000) == 200 && res.length == 0, "HEAD: no body, whatever Content-Length says");
        check(http.get(url("/nope").c_str(), buf, sizeof buf, res, 3000) == 404, "404 is a response, not an error");
        check(http.connected() && r.srv->connects() == 1, "all of it on one kept connection");

        check(http.get(url("/close").c_str(), buf, sizeof buf, res, 3000) == 200 &&
                  std::strcmp(reinterpret_cast<char*>(buf), "bye now") == 0,
              "a body that runs to the close");
        check(!http.connected(), "and the connection is gone");
        check(http.get(url("/http10").c_str(), buf, sizeof buf, res, 3000) == 200 && !http.connected() &&
                  r.srv->connects() == 2,
              "HTTP/1.0: a new connection, not kept");
        check(http.get("http://192.168.1.20:8080/hello", buf, sizeof buf, res, 3000) == 200 && r.srv->connects() == 3,
              "by address");
        check(http.get("http://192.168.1.20:8081/hello", buf, sizeof buf, res, 3000) == 200 && r.srv->connects() == 4,
              "another port: another connection");
    }

    std::printf("big bodies\n");
    {
        Rig r;
        xHttpClient http(*r.eth);
        uint8_t small[64];
        xHttpClient::Response res;
        check(http.get(url("/big").c_str(), small, sizeof small, res, 5000) == 200, "20 KB into 64 bytes: 200");
        check(res.length == 20000 && res.truncated && std::memcmp(small, pattern(64).data(), 64) == 0,
              "its start kept, the rest read and dropped");
        check(http.connected(), "the connection still good for the next request");

        Streamed st;
        xHttpClient::Request q;
        const std::string bigUrl = url("/big");
        q.url = bigUrl.c_str();
        q.onBody = Streamed::fn;
        q.ctx = &st;
        check(http.request(q, res, 5000) == 200 && st.data == pattern(20000), "20 KB streamed to onBody, every byte");
        check(r.srv->connects() == 1, "on the same connection");

        Streamed stop;
        stop.stopAt = 1000;
        q.ctx = &stop;
        check(http.request(q, res, 5000) == 0 && http.error() == xHttpClient::Error::Aborted && !http.connected(),
              "onBody saying stop: Aborted, connection dropped");
        check(http.get(url("/hello").c_str(), small, sizeof small, res, 3000) == 200, "the next request reconnects");
    }

    std::printf("kept connections going stale\n");
    {
        Rig r;
        xHttpClient http(*r.eth);
        uint8_t buf[64];
        xHttpClient::Response res;
        http.get(url("/hello").c_str(), buf, sizeof buf, res, 3000);
        r.srv->closeAll();   // the server's idle timeout
        sleepMs(200);
        check(http.get(url("/hello").c_str(), buf, sizeof buf, res, 3000) == 200 && r.srv->connects() == 2,
              "closed by the server while idle: a new connection, no error");

        check(http.get(url("/dropOnce").c_str(), buf, sizeof buf, res, 3000) == 200 &&
                  std::strcmp(reinterpret_cast<char*>(buf), "second") == 0 && r.srv->connects() == 3,
              "closed as a GET arrived: sent again on a new connection");

        const size_t before = r.srv->requests().size();
        check(http.post(url("/drop").c_str(), "text/plain", "x", 1, buf, sizeof buf, res, 3000) == 0 &&
                  http.error() == xHttpClient::Error::Closed,
              "closed as a POST arrived: Closed, not sent again");
        check(r.srv->requests().size() == before + 1, "the server saw it once");
    }

    std::printf("failures\n");
    {
        Rig r;
        xHttpClient http(*r.eth);
        uint8_t buf[64];
        xHttpClient::Response res;
        const auto t0 = steady_clock::now();
        check(http.get(url("/silent").c_str(), buf, sizeof buf, res, 300) == 0 &&
                  http.error() == xHttpClient::Error::Timeout,
              "no answer: Timeout");
        const long took = msSince(t0);
        check(took >= 280 && took < 800 && !http.connected(), "after the timeout, connection dropped");

        check(http.get(url("/garbage").c_str(), buf, sizeof buf, res, 2000) == 0 &&
                  http.error() == xHttpClient::Error::Protocol && !http.connected(),
              "a malformed response: Protocol");
        check(http.get("http://nowhere.local/", buf, sizeof buf, res, 2000) == 0 &&
                  http.error() == xHttpClient::Error::Resolve,
              "an unknown host: Resolve");
        check(http.get("https://sensors.local/", buf, sizeof buf, res, 2000) == 0 &&
                  http.error() == xHttpClient::Error::Unsupported,
              "https: Unsupported");
        check(http.get("ftp://sensors.local/", buf, sizeof buf, res, 2000) == 0 &&
                  http.error() == xHttpClient::Error::BadUrl,
              "not http: BadUrl");
        const std::string longHost = "http://" + std::string(70, 'h') + "/";
        check(http.get(longHost.c_str(), buf, sizeof buf, res, 2000) == 0 && http.error() == xHttpClient::Error::BadUrl,
              "a host name over 63 characters: BadUrl");
        check(http.get(url("/hello").c_str(), buf, sizeof buf, res, 2000) == 200, "and then a good request");
    }
    {
        Rig r(true);
        xHttpClient http(*r.eth);
        uint8_t buf[64];
        xHttpClient::Response res;
        check(http.get(url("/hello").c_str(), buf, sizeof buf, res, 2000) == 0 &&
                  http.error() == xHttpClient::Error::Connect,
              "refused: Connect");
        check(http.get(url("/hello").c_str(), buf, sizeof buf, res, 2000) == 0 &&
                  http.error() == xHttpClient::Error::Connect,
              "again, without running out of sockets");
    }
    {
        Rig r;
        xHttpClient::Config c;
        c.keepAlive = false;
        c.userAgent = nullptr;
        xHttpClient http(*r.eth, c);
        uint8_t buf[64];
        xHttpClient::Response res;
        check(http.get(url("/hello").c_str(), buf, sizeof buf, res, 3000) == 200 && !http.connected(),
              "keepAlive off: closed after each request");
        auto reqs = r.srv->requests();
        check(has(reqs[0].head, "Connection: close\r\n") && !has(reqs[0].head, "User-Agent"),
              "says Connection: close, no User-Agent");
    }

    std::printf("%s\n", g_failures ? "FAILED" : "all passed");
    return g_failures ? 1 : 0;
}
