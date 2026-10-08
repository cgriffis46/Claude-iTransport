// Host test for changing the UI without new firmware, end to end:
// xHttpServer over HTTPS (MbedTlsServer) on SocketNetDevice, WebAuth with
// PBKDF2 logins, HttpFileAdmin, and HttpStaticFiles serving LittleFS on a
// simulated SPI flash chip first and the built-in pages after it. curl
// logs in as an admin, uploads a page in pieces, and fetches it back;
// the flash is then mounted afresh and the page is still there.
//
//   StorageWeb_test --serve 120   also stays up that long afterwards, for
//                                 storage/test/files_ui_test.cjs (Chromium)
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
#include "HttpFileAdmin.h"
#include "HttpJson.h"
#include "MbedTlsServer.h"
#include "TlsFreeRtos.h"
#include "WebAuth.h"
#include "WebPassword.h"
#include "SpiNorFlash.h"
#include "LittleFsNor.h"
#include "HttpLittleFsFiles.h"
#include "SimSpiNor.h"
#include "TestCerts.h"

using namespace std::chrono;

static int g_failures = 0;
static void check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failures;
}
static bool has(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }
static std::string run(const std::string& cmd) {
    std::string out;
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return out;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, p)) > 0) out.append(buf, n);
    pclose(p);
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
static uint32_t msClock(void*) {
    return static_cast<uint32_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}
static void yieldFn(void*) { std::this_thread::yield(); }
static std::string tempFile(const std::string& data) {
    char name[] = "/tmp/storageweb_XXXXXX";
    const int fd = mkstemp(name);
    if (fd >= 0) {
        if (::write(fd, data.data(), data.size()) < 0) {}
        ::close(fd);
    }
    return name;
}

static std::mutex g_auditLock;
static std::string g_audit;
static void audit(const char* user, const char* action, const char* path, size_t size, void*) {
    std::lock_guard<std::mutex> g(g_auditLock);
    g_audit += std::string(user) + " " + action + " " + path + " " + std::to_string(size) + "\n";
}

static const uint8_t kIndex[] = "<!doctype html><title>built in</title><p>the firmware's own page</p>";

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const int serveS = argc >= 3 && std::strcmp(argv[1], "--serve") == 0 ? std::atoi(argv[2]) : 0;

    std::printf("the flash\n");
    SimSpiNor chip(0xEF4017, 8u << 20);   // W25Q64: 8 MB
    FakeNorBus bus(chip);
    SpiNorFlash::Config fc;
    fc.yield = yieldFn;
    fc.now = msClock;
    SpiNorFlash flash(bus, fc);
    TlsFreeRtosLock tlsLock, authLock, fsLock;
    LittleFsNor::Config lc;
    lc.lock = &fsLock;
    lc.firstSector = 256;   // the first 1 MB for something else (firmware, settings)
    lc.sectors = 1792;
    LittleFsNor fs(flash, lc);
    check(flash.begin() && fs.mount(), "W25Q64 found; LittleFS formatted on its last 7 MB and mounted");
    HttpLittleFsFiles files(fs, "/www");

    std::printf("the server\n");
    MbedTlsServer::Config tc;
    tc.certPem = testCertPem;
    tc.keyPem = testKeyPem;
    tc.sessions = 2;
    tc.lock = &tlsLock;
    MbedTlsServer tls(tc);
    check(tls.begin(), "TLS up");
    WebUser users[2] = {};
    users[0].name = "admin";
    users[0].role = WebRole::Admin;
    users[1].name = "operator";
    users[1].role = WebRole::Operator;
    for (WebUser& u : users) {
        u.iterations = 1000;
        tls.random(u.salt, 16);
    }
    WebPassword::hash("admin-password", users[0].salt, 16, 1000, users[0].hash);
    WebPassword::hash("op-password", users[1].salt, 16, 1000, users[1].hash);
    WebAuth::Config ac;
    ac.users = users;
    ac.userCount = 2;
    ac.verify = WebPassword::verify;
    ac.random = MbedTlsServer::randomFn;
    ac.randomCtx = &tls;
    ac.now = msClock;
    ac.lock = &authLock;
    WebAuth auth(ac);

    HttpFileAdmin::Config fa;
    fa.auth = &auth;
    fa.audit = audit;
    fa.sleep = [](uint32_t ms, void*) { std::this_thread::sleep_for(milliseconds(ms)); };
    HttpFileAdmin admin(files, fa);
    static const HttpMemoryFiles::File builtIn[] = {{"/index.html", kIndex, sizeof kIndex - 1}, httpFileAdminPage};
    HttpMemoryFiles firmware(builtIn, 2);
    HttpStaticFiles site(files, &firmware);   // the flash first, then the firmware's

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

    xHttpServer::Config sc;
    sc.port = freePort();
    sc.maxClients = 3;   // a browser keeps two connections open, curl one more
    sc.tls = &tls;
    sc.idleTimeoutMs = 1000;
    xHttpServer web(eth, sc);
    check(auth.attach(web) && admin.attach(web) && web.get("/*", HttpStaticFiles::handler, &site), "routes");
    web.begin();
    std::thread daemon([&] { web.run(); });

    const std::string caFile = tempFile(testCaPem), jar = tempFile("");
    const std::string base = "https://127.0.0.1:" + std::to_string(sc.port);
    const std::string curl = "curl -s --max-time 10 --cacert " + caFile + " -b " + jar + " -c " + jar + " ";

    std::printf("before any upload\n");
    std::string out;
    for (int i = 0; i < 100 && (out = run(curl + base + "/")).empty(); ++i) std::this_thread::sleep_for(milliseconds(20));
    check(has(out, "the firmware's own page"), "/: the built-in page (the flash is empty)");
    check(has(run(curl + base + "/files.html"), "<title>Files</title>"), "/files.html: the upload page, built in");
    check(has(run(curl + base + "/api/files"), "login required"), "/api/files without a login: refused");

    std::printf("an operator, then an admin\n");
    auto login = [&](const char* user, const char* pw) {
        const std::string r = run(curl + "-H 'Content-Type: application/json' -d '{\"user\":\"" + user + "\",\"password\":\"" +
                                  pw + "\"}' " + base + "/api/login");
        char csrf[40] = "";
        HttpJson::string(r.data(), r.size(), "csrf", csrf, sizeof csrf);
        return std::string(csrf);
    };
    std::string csrf = login("operator", "op-password");
    check(csrf.size() == 32 && has(run(curl + "-X PUT -H 'X-CSRF-Token: " + csrf + "' --data-binary x '" + base +
                                       "/api/files/index.html?offset=0'"), "not allowed"),
          "an operator's upload: refused (needs admin)");
    csrf = login("admin", "admin-password");
    check(csrf.size() == 32, "an admin logged in");
    const std::string tok = "-H 'X-CSRF-Token: " + csrf + "' ";

    std::printf("a new UI, uploaded\n");
    std::string page = "<!doctype html><title>uploaded</title><p>a new UI, no firmware change</p>\n";
    while (page.size() < 20000) page += "<!-- " + std::to_string(page.size()) + " padding to make it several pieces -->\n";
    const std::string pageFile = tempFile(page);
    bool ok = true;
    for (size_t at = 0; at < page.size(); at += 1024) {
        const std::string piece = tempFile(page.substr(at, 1024));
        const std::string r = run(curl + "-X PUT " + tok + "-H 'Content-Type: application/octet-stream' --data-binary @" +
                                  piece + " '" + base + "/api/files/index.html?offset=" + std::to_string(at) + "'");
        std::remove(piece.c_str());
        ok = ok && r == "{\"received\":" + std::to_string(std::min(at + 1024, page.size())) + "}";
    }
    check(ok, "20 KB in 1 KB PUTs over HTTPS");
    check(has(run(curl + base + "/"), "the firmware's own page"), "before the commit: still the built-in page");
    check(run(curl + "-X POST " + tok + "-d '' '" + base + "/api/files/index.html?size=" + std::to_string(page.size()) + "'") == "{}",
          "committed");
    check(run(curl + base + "/") == page, "/: now the uploaded page, whole");
    const std::string list = run(curl + base + "/api/files");
    check(has(list, "{\"path\":\"/index.html\",\"size\":" + std::to_string(page.size()) + "}") && has(list, "\"free\":"),
          "listed, with the space");
    check(has(g_audit, "admin upload /index.html 0\nadmin commit /index.html " + std::to_string(page.size())), "audited");

    std::printf("replaced while someone downloads it\n");
    {
        std::string v2 = "<!doctype html><title>v2</title>\n";
        while (v2.size() < page.size()) v2 += "<!-- the second version -->\n";
        bool sent = true;
        for (size_t at = 0; at < v2.size(); at += 1024) {
            const std::string piece = tempFile(v2.substr(at, 1024));
            sent = sent && has(run(curl + "-X PUT " + tok + "--data-binary @" + piece + " '" + base +
                                   "/api/files/index.html?offset=" + std::to_string(at) + "'"), "received");
            std::remove(piece.c_str());
        }
        // A slow client downloading the page while the new one is committed.
        // On a PC the kernel's socket buffers take the whole page at once, so
        // the server's file is closed again at once and the commit rarely
        // waits; on the device (2 KB W5500 buffers, lwIP's small ones) it
        // does. The waiting itself is checked in SpiNorFlash_test and
        // HttpFatFs_test, and HttpFileAdmin_test.
        const std::string slowOut = tempFile("");
        std::thread slow([&] { run("curl -s --max-time 20 --limit-rate 8k --cacert " + caFile + " " + base + "/ -o " + slowOut); });
        std::this_thread::sleep_for(milliseconds(300));
        const std::string committed = run(curl + "-X POST " + tok + "-d '' '" + base + "/api/files/index.html?size=" +
                                          std::to_string(v2.size()) + "'");
        slow.join();
        FILE* f = std::fopen(slowOut.c_str(), "rb");
        std::string got;
        if (f) {
            char b[4096];
            size_t n;
            while ((n = std::fread(b, 1, sizeof b, f)) > 0) got.append(b, n);
            std::fclose(f);
        }
        std::remove(slowOut.c_str());
        check(sent && committed == "{}", "committed during a slow download");
        check(got == page, "the slow download got the old page, whole");
        check(run(curl + base + "/") == v2, "then the new page");
        page = v2;
    }

    std::printf("after a restart\n");
    {
        LittleFsNor again(flash, lc);
        HttpLittleFsFiles files2(again, "/www");
        check(again.mount(), "mounted afresh from the chip");
        size_t size = 0;
        void* f = files2.open("/index.html", size);
        std::string back(size, 0);
        if (f) {
            files2.read(f, 0, reinterpret_cast<uint8_t*>(&back[0]), size);
            files2.close(f);
        }
        check(f && back == page, "the uploaded page is there");
        again.unmount();
    }
    check(run(curl + "-X DELETE " + tok + base + "/api/files/index.html") == "{}" &&
              has(run(curl + base + "/"), "the firmware's own page"),
          "deleted: the built-in page is back");
    std::printf("  (flash: %lu sector erases, %lu programs)\n", static_cast<unsigned long>(fs.erases()),
                static_cast<unsigned long>(fs.programs()));

    if (serveS > 0) {
        std::printf("serving %s for %d s (admin / admin-password, operator / op-password)\n", base.c_str(), serveS);
        std::this_thread::sleep_for(seconds(serveS));
    }

    web.stop();
    daemon.join();
    quit = true;
    driver.join();
    std::remove(caFile.c_str());
    std::remove(jar.c_str());
    std::remove(pageFile.c_str());
    std::printf("%s\n", g_failures ? "FAILED" : "all passed");
    return g_failures ? 1 : 0;
}
