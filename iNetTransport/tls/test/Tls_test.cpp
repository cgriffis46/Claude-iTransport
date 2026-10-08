// Host test for HTTPS: xHttpServer with MbedTlsServer (mbedTLS 3.6, built
// with tls/config/inet_mbedtls_config.h) on SocketNetDevice over the
// PC's sockets, checked by curl and openssl s_client: the protocol and
// cipher suite, refusal of old versions and of plain HTTP, ticket
// resumption, the redirect from port 80, a login with PBKDF2 and a
// protected write, a streamed page, two connections at once, and the
// heap a TLS connection actually takes (measured through mbedTLS's
// calloc).
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include "NetSockets.h"
#include "SocketNetDevice.h"
#include "xEthernet.h"
#include "xHttpServer.h"
#include "HttpRedirect.h"
#include "HttpJson.h"
#include "MbedTlsServer.h"
#include "TlsFreeRtos.h"
#include "WebAuth.h"
#include "WebPassword.h"
#include "TestCerts.h"
#include "mbedtls/platform.h"

using namespace std::chrono;

static int g_failures = 0;
static void check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failures;
}
static bool has(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }
static long msSince(steady_clock::time_point t0) {
    return static_cast<long>(duration_cast<milliseconds>(steady_clock::now() - t0).count());
}
static std::string run(const std::string& cmd, int* status = nullptr) {
    std::string out;
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return out;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, p)) > 0) out.append(buf, n);
    const int st = pclose(p);
    if (status) *status = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    return out;
}
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
static std::string hex(const uint8_t* b, size_t n) {
    std::string s;
    char h[3];
    for (size_t i = 0; i < n; ++i) { std::snprintf(h, sizeof h, "%02x", b[i]); s += h; }
    return s;
}

// ---- mbedTLS's heap, counted ----
static std::mutex g_heapLock;
static size_t g_heapNow = 0, g_heapPeak = 0;
static void* countingCalloc(size_t n, size_t size) {
    const size_t bytes = n * size;
    size_t* p = static_cast<size_t*>(std::calloc(1, bytes + sizeof(size_t) * 2));
    if (!p) return nullptr;
    p[0] = bytes;
    std::lock_guard<std::mutex> g(g_heapLock);
    g_heapNow += bytes;
    if (g_heapNow > g_heapPeak) g_heapPeak = g_heapNow;
    return p + 2;
}
static void countingFree(void* q) {
    if (!q) return;
    size_t* p = static_cast<size_t*>(q) - 2;
    {
        std::lock_guard<std::mutex> g(g_heapLock);
        g_heapNow -= p[0];
    }
    std::free(p);
}
static size_t heapNow() { std::lock_guard<std::mutex> g(g_heapLock); return g_heapNow; }
static size_t heapPeak() { std::lock_guard<std::mutex> g(g_heapLock); return g_heapPeak; }
static void resetPeak() { std::lock_guard<std::mutex> g(g_heapLock); g_heapPeak = g_heapNow; }

// ---- the site ----
static uint32_t msClock(void*) {
    return static_cast<uint32_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}
static WebAuth* g_auth;
static std::atomic<int> g_valve{0};
static void hello(const HttpRequest& req, HttpResponse& res, void*) {
    res.send("text/plain", req.secure() ? "hello over TLS" : "hello in the clear");
}
static void valve(const HttpRequest& req, HttpResponse& res, void*) {
    WebAuth::Identity who;
    if (!g_auth->require(req, res, WebRole::Operator, true, &who)) return;
    ++g_valve;
    res.send("text/plain", (std::string("opened by ") + who.user).c_str());
}
static void big(const HttpRequest&, HttpResponse& res, void*) {
    res.begin("text/plain");
    for (int i = 0; i < 3000; ++i) res.printf("line %04d of a page sent through TLS\n", i);
}
static void slow(const HttpRequest&, HttpResponse& res, void*) {
    std::this_thread::sleep_for(milliseconds(300));
    res.send("text/plain", "slow");
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    mbedtls_platform_set_calloc_free(countingCalloc, countingFree);

    std::printf("passwords\n");
    {
        uint8_t h[32];
        WebPassword::hash("password", reinterpret_cast<const uint8_t*>("salt"), 4, 1, h);
        check(hex(h, 32) == "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b", "PBKDF2-SHA256, c=1 (RFC 7914 vector)");
        WebPassword::hash("password", reinterpret_cast<const uint8_t*>("salt"), 4, 4096, h);
        check(hex(h, 32) == "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a", "c=4096");
        // An entry from tools/web_user.py (Python's hashlib) verifies here.
        const std::string line = run("python3 ../tools/web_user.py ann operator --password 'correct horse' --iterations 2000 2>&1");
        WebUser u{};
        u.name = "ann";
        u.role = WebRole::Operator;
        unsigned v[48];
        const char* s = std::strstr(line.c_str(), "2000, {");
        int got = s ? std::sscanf(s, "2000, {0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x, "
                                     "0x%x, 0x%x}, {0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x, "
                                     "0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x, "
                                     "0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x}",
                                  &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9], &v[10], &v[11],
                                  &v[12], &v[13], &v[14], &v[15], &v[16], &v[17], &v[18], &v[19], &v[20], &v[21], &v[22],
                                  &v[23], &v[24], &v[25], &v[26], &v[27], &v[28], &v[29], &v[30], &v[31], &v[32], &v[33],
                                  &v[34], &v[35], &v[36], &v[37], &v[38], &v[39], &v[40], &v[41], &v[42], &v[43], &v[44],
                                  &v[45], &v[46], &v[47])
                    : 0;
        u.iterations = 2000;
        for (int i = 0; i < 16; ++i) u.salt[i] = static_cast<uint8_t>(v[i]);
        for (int i = 0; i < 32; ++i) u.hash[i] = static_cast<uint8_t>(v[16 + i]);
        check(got == 48 && WebPassword::verify(u, "correct horse", nullptr) && !WebPassword::verify(u, "correct horsf", nullptr),
              "tools/web_user.py's entry: the right password only");
    }

    std::printf("TLS server\n");
    TlsFreeRtosLock tlsLock, authLock;
    MbedTlsServer::Config bad;
    bad.certPem = testCertPem;
    bad.keyPem = "-----BEGIN EC PRIVATE KEY-----\nnot a key\n-----END EC PRIVATE KEY-----\n";
    MbedTlsServer broken(bad);
    check(!broken.begin() && broken.error() != 0, "a bad key: begin() fails, with mbedTLS's code");
    MbedTlsServer::Config tc;
    tc.certPem = testCertPem;
    tc.keyPem = testKeyPem;
    tc.sessions = 2;
    tc.lock = &tlsLock;
    MbedTlsServer tls(tc);
    check(tls.begin(), "begin(): certificate and key loaded, generator seeded");

    // Users: real PBKDF2 hashes (few iterations: it's a test).
    WebUser users[2] = {};
    users[0].name = "ann";
    users[0].role = WebRole::Operator;
    users[0].iterations = 1000;
    users[1].name = "vic";
    users[1].role = WebRole::Viewer;
    users[1].iterations = 1000;
    tls.random(users[0].salt, 16);
    tls.random(users[1].salt, 16);
    WebPassword::hash("ann-secret", users[0].salt, 16, 1000, users[0].hash);
    WebPassword::hash("vic-secret", users[1].salt, 16, 1000, users[1].hash);
    WebAuth::Config ac;
    ac.users = users;
    ac.userCount = 2;
    ac.verify = WebPassword::verify;
    ac.random = MbedTlsServer::randomFn;
    ac.randomCtx = &tls;
    ac.now = msClock;
    ac.lock = &authLock;
    WebAuth auth(ac);
    g_auth = &auth;

    SocketNetDevice dev;
    xNetInterface::Config ncfg;
    ncfg.maxSockets = 8;
    ncfg.ntpIntervalMs = 0;
    xEthernet eth(dev, ncfg);
    NetConfig net;
    net.ip = IpAddress(127, 0, 0, 1);
    net.subnet = IpAddress(255, 0, 0, 0);
    eth.begin(net);
    std::atomic<bool> quit{false};
    std::thread driver([&] { while (!quit) eth.service(20); });
    eth.waitAddress(2000);

    const uint16_t httpsPort = freePort(), httpPort = freePort();
    xHttpServer::Config sc;
    sc.port = httpsPort;
    sc.maxClients = 2;
    sc.tls = &tls;
    sc.idleTimeoutMs = 1000;   // openssl s_client stays until the server closes
    xHttpServer web(eth, sc);
    web.get("/hello", hello);
    web.post("/api/valve", valve);
    web.get("/big", big);
    web.get("/slow", slow);
    auth.attach(web);
    web.begin();
    std::thread daemon([&] { web.run(); });

    HttpsRedirect toHttps(httpsPort);
    xHttpServer::Config pc;
    pc.port = httpPort;
    pc.maxClients = 1;
    xHttpServer plain(eth, pc);
    plain.get("/*", HttpsRedirect::handler, &toHttps);
    plain.begin();
    std::thread plainDaemon([&] { plain.run(); });

    char caFile[] = "/tmp/tlsca_XXXXXX";
    const int caFd = mkstemp(caFile);
    if (caFd < 0 || ::write(caFd, testCaPem, std::strlen(testCaPem)) < 0) return 1;
    ::close(caFd);
    const std::string base = "https://127.0.0.1:" + std::to_string(httpsPort);
    const std::string curl = std::string("curl -s --max-time 5 --cacert ") + caFile + " ";

    std::printf("HTTPS, to curl and openssl\n");
    std::string out;
    for (int i = 0; i < 100 && (out = run(curl + base + "/hello")).empty(); ++i) std::this_thread::sleep_for(milliseconds(20));
    check(out == "hello over TLS", "curl, checking the certificate against our CA: answered, and the request knew it was TLS");
    int st = 0;
    run("curl -s --max-time 5 " + base + "/hello", &st);
    check(st == 60, "without our CA: curl refuses the certificate (as a browser warns)");
    check(run(curl + "https://localhost:" + std::to_string(httpsPort) + "/hello") == "hello over TLS", "by the name in the certificate too");

    const std::string brief = run("echo | timeout 10 openssl s_client -connect 127.0.0.1:" + std::to_string(httpsPort) +
                                  " -CAfile " + caFile + " -brief 2>&1");
    check(has(brief, "Protocol version: TLSv1.2") && has(brief, "Ciphersuite: ECDHE-ECDSA-AES128-GCM-SHA256"),
          "TLS 1.2, ECDHE-ECDSA-AES128-GCM-SHA256");
    check(has(brief, "Verification: OK"), "the chain verifies");
    const std::string x25519 = run("echo | timeout 10 openssl s_client -connect 127.0.0.1:" + std::to_string(httpsPort) +
                                   " -CAfile " + caFile + " -groups X25519:P-256 -brief 2>&1");
    // TLS 1.2: the client's groups must include the certificate's curve
    // (P-256), as every browser's do; then X25519 is chosen for ECDHE.
    check(has(x25519, "Server Temp Key: X25519"), "X25519 for the key exchange, when offered with P-256 (as browsers do)");
    const std::string chacha = run("echo | timeout 10 openssl s_client -connect 127.0.0.1:" + std::to_string(httpsPort) +
                                   " -CAfile " + caFile + " -tls1_2 -cipher ECDHE-ECDSA-CHACHA20-POLY1305 -brief 2>&1");
    check(has(chacha, "ECDHE-ECDSA-CHACHA20-POLY1305"), "and ChaCha20-Poly1305, for clients without AES hardware");

    const uint32_t failsBefore = web.stats().tlsFailures;
    run(curl + "--tlsv1.0 --tls-max 1.1 " + base + "/hello", &st);
    check(st != 0, "TLS 1.0/1.1: refused");
    run("curl -s --max-time 3 http://127.0.0.1:" + std::to_string(httpsPort) + "/hello", &st);
    check(st != 0, "plain HTTP on the TLS port: refused");
    const std::string weak = run("echo | timeout 10 openssl s_client -connect 127.0.0.1:" + std::to_string(httpsPort) +
                                 " -tls1_2 -cipher AES128-SHA -brief 2>&1");
    check(!has(weak, "Ciphersuite"), "a suite without forward secrecy: refused");
    std::this_thread::sleep_for(milliseconds(100));
    check(web.stats().tlsFailures >= failsBefore + 3, "each counted as a failed handshake");

    // A returning client: the ticket skips the key exchange.
    char sess[] = "/tmp/tlssess_XXXXXX";
    const int sessFd = mkstemp(sess);
    ::close(sessFd);
    const std::string sc1 = "echo | timeout 10 openssl s_client -connect 127.0.0.1:" + std::to_string(httpsPort) + " -CAfile " +
                            caFile + " -tls1_2 ";
    run(sc1 + "-sess_out " + sess + " 2>&1");
    const std::string again = run(sc1 + "-sess_in " + sess + " 2>&1");
    check(has(again, "Reused, TLSv1.2"), "session ticket: the second connection resumes");

    std::printf("the redirect on port 80\n");
    const std::string head = run("curl -sI --max-time 3 'http://127.0.0.1:" + std::to_string(httpPort) + "/plc/tags?x=1'");
    check(has(head, "HTTP/1.1 301") && has(head, "Location: https://127.0.0.1:" + std::to_string(httpsPort) + "/plc/tags?x=1"),
          "301 to the same path on https");
    check(run(curl + "-L http://127.0.0.1:" + std::to_string(httpPort) + "/hello") == "hello over TLS",
          "curl -L follows it to the TLS server");

    std::printf("login, over HTTPS\n");
    char jar[] = "/tmp/tlsjar_XXXXXX";
    const int jarFd = mkstemp(jar);
    ::close(jarFd);
    const std::string cj = curl + "-c " + jar + " -b " + jar + " ";
    std::string r = run(cj + "-D - -H 'Content-Type: application/json' -d '{\"user\":\"ann\",\"password\":\"ann-secret\"}' " +
                        base + "/api/login");
    check(has(r, "HTTP/1.1 200") && has(r, "HttpOnly; SameSite=Strict; Max-Age=28800; Secure"),
          "logged in: a Secure, HttpOnly, SameSite=Strict cookie");
    const std::string body = r.substr(r.find("\r\n\r\n") + 4);
    char csrf[40] = "";
    HttpJson::string(body.data(), body.size(), "csrf", csrf, sizeof csrf);
    check(std::strlen(csrf) == 32, "and a CSRF token");
    check(run(cj + "-X POST -H 'X-CSRF-Token: " + csrf + "' " + base + "/api/valve") == "opened by ann" && g_valve == 1,
          "a protected write, with the cookie and the token");
    check(has(run(cj + "-X POST " + base + "/api/valve"), "CSRF") && g_valve == 1, "without the token: refused");
    check(has(run(curl + "-X POST -H 'X-CSRF-Token: " + csrf + "' " + base + "/api/valve"), "login required") && g_valve == 1,
          "without the cookie: refused");
    check(has(run(curl + "-H 'Content-Type: application/json' -d '{\"user\":\"ann\",\"password\":\"guess\"}' " + base +
                  "/api/login"), "wrong user or password"),
          "a wrong password");
    {
        const std::string r80 = run("curl -s -D - --max-time 3 -H 'Content-Type: application/json' -d '{\"user\":\"ann\","
                                    "\"password\":\"ann-secret\"}' http://127.0.0.1:" + std::to_string(httpPort) + "/api/login");
        check(!has(r80, "200 OK") && !has(r80, "Set-Cookie"), "a login sent to port 80: no session (405 there)");
    }
    run(cj + "-X POST " + base + "/api/logout");
    check(has(run(cj + "-X POST -H 'X-CSRF-Token: " + std::string(csrf) + "' " + base + "/api/valve"), "login required"),
          "after logout: refused");

    std::printf("bigger, and at once\n");
    const std::string page = run(curl + base + "/big");
    check(page.size() == 3000 * 37 && has(page, "line 2999 of a page sent through TLS"), "111 KB streamed through TLS, whole");
    const auto t0 = steady_clock::now();
    const std::string two = run("(" + curl + base + "/slow & " + curl + base + "/slow & wait)");
    check(two == "slowslow" && msSince(t0) < 1500, "two TLS connections at once (the server's two sessions)");

    std::printf("memory\n");
    {
        // Every earlier connection closed first: their buffers are freed then.
        for (int i = 0; i < 200 && web.stats().active != 0; ++i) std::this_thread::sleep_for(milliseconds(10));
        std::this_thread::sleep_for(milliseconds(50));
        const size_t before = heapNow();
        resetPeak();
        // Under load, a connection can be turned away while the server is
        // between accepts: try again until one is answered.
        for (int i = 0; i < 5 && run(curl + base + "/hello") != "hello over TLS"; ++i) {
            std::this_thread::sleep_for(milliseconds(100));
        }
        for (int i = 0; i < 200 && web.stats().active != 0; ++i) std::this_thread::sleep_for(milliseconds(10));
        std::this_thread::sleep_for(milliseconds(50));
        const size_t peak = heapPeak() - before;
        std::printf("  (one TLS connection's heap at its peak: %zu bytes; held by the server between: %zu)\n", peak, before);
        check(peak > 16384 && peak < 40000, "one connection: its record buffers and state, under 40 KB");
        check(heapNow() == before, "all of it back when the connection closes");
    }

    web.stop();
    plain.stop();
    daemon.join();
    plainDaemon.join();
    quit = true;
    driver.join();
    std::remove(caFile);
    std::remove(sess);
    std::remove(jar);
    std::printf("  (handshakes %u, failed %u)\n", tls.handshakes(), tls.handshakeFailures());
    std::printf("%s\n", g_failures ? "FAILED" : "all passed");
    return g_failures ? 1 : 0;
}
