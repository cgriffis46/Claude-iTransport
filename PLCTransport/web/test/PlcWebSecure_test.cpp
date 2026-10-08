// End-to-end host test of secure tag writes: the PLC's tags behind
// xHttpServer with HTTPS (MbedTlsServer), WebAuth logins with PBKDF2
// passwords, PlcTagWebApi with an allow-list and an audit log, and the
// redirect from plain HTTP, over SocketNetDevice on the PC's sockets,
// driven by curl. A "control loop" thread keeps working on the tags.
//
//   PlcWebSecure_test               runs the checks
//   PlcWebSecure_test --serve 120   also stays up that long afterwards,
//                                   printing its URL, for a browser
//
// Built by CMake when MBEDTLS_DIR is set (HOST builds).
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>
#include "NetSockets.h"
#include "SocketNetDevice.h"
#include "xEthernet.h"
#include "xHttpServer.h"
#include "HttpFiles.h"
#include "HttpJson.h"
#include "HttpRedirect.h"
#include "MbedTlsServer.h"
#include "TlsFreeRtos.h"
#include "WebAuth.h"
#include "WebPassword.h"
#include "PlcTagWebApi.h"
#include "PlcWebDefaultPage.h"
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

struct AuditLog {
    std::mutex m;
    std::string text;
    static void fn(const char* who, const PlcTagSnapshot& before, const PlcTagSnapshot& after, void* ctx) {
        char b[80], a[80];
        PlcTagWebApi::formatValue(before, b, sizeof b);
        PlcTagWebApi::formatValue(after, a, sizeof a);
        AuditLog& log = *static_cast<AuditLog*>(ctx);
        std::lock_guard<std::mutex> g(log.m);
        log.text += std::string(who) + " set " + before.name + " " + b + " -> " + a + "\n";
    }
    std::string get() { std::lock_guard<std::mutex> g(m); return text; }
};

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const int serveS = argc >= 3 && std::strcmp(argv[1], "--serve") == 0 ? std::atoi(argv[2]) : 0;
    tlsUseFreeRtosHeap();   // the stub's pvPortMalloc: malloc

    // The PLC.
    PlcTagRegistry reg;
    float setpoint = 60.0f;
    float temp = 20.0f;
    bool pump = false;
    int32_t count = 0;
    bool estop = false;
    reg.registerTag("Oven.Setpoint", setpoint);
    reg.registerTag("Oven.Temp", temp, false);
    reg.registerTag("Oven.Pump", pump);
    reg.registerTag("Oven.Count", count, false);
    reg.registerTag("Safety.EStop", estop);   // writable by the CIP server, never by the web
    std::atomic<bool> stop{false};
    std::thread control([&] {
        while (!stop) {
            float sp = 0, t = 0;
            reg.readTag("Oven.Setpoint", &sp, sizeof sp);
            reg.readTag("Oven.Temp", &t, sizeof t);
            t += (sp - t) * 0.05f;
            // As a control program does: its own variables, directly. (The
            // registry's read-only flag is for writes through it.)
            temp = t;
            ++count;
            std::this_thread::sleep_for(milliseconds(20));
        }
    });

    // TLS and logins.
    TlsFreeRtosLock tlsLock, authLock;
    MbedTlsServer::Config tc;
    tc.certPem = testCertPem;
    tc.keyPem = testKeyPem;
    tc.sessions = 3;
    tc.lock = &tlsLock;
    MbedTlsServer tls(tc);
    check(tls.begin(), "TLS server up");
    WebUser users[2] = {};
    users[0] = WebUser{"operator", WebRole::Operator, 2000, {}, {}};
    users[1] = WebUser{"viewer", WebRole::Viewer, 2000, {}, {}};
    for (WebUser& u : users) tls.random(u.salt, sizeof u.salt);
    WebPassword::hash("op-password", users[0].salt, 16, 2000, users[0].hash);
    WebPassword::hash("view-password", users[1].salt, 16, 2000, users[1].hash);
    WebAuth::Config ac;
    ac.users = users;
    ac.userCount = 2;
    ac.verify = WebPassword::verify;
    ac.random = MbedTlsServer::randomFn;
    ac.randomCtx = &tls;
    ac.now = msClock;
    ac.lock = &authLock;
    WebAuth auth(ac);

    const PlcWebWritable writable[] = {{"Oven.Setpoint", 20, 250}, {"Oven.Pump", 0, 0}};
    AuditLog audit;
    PlcTagWebApi::Config pc;
    pc.auth = &auth;
    pc.writable = writable;
    pc.writableCount = 2;
    pc.audit = AuditLog::fn;
    pc.auditCtx = &audit;
    PlcTagWebApi api(reg, pc);

    // The network and the two servers.
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

    HttpMemoryFiles builtIn(plcWebDefaultFiles, plcWebDefaultFileCount);
    HttpStaticFiles site(builtIn);
    xHttpServer::Config sc;
    sc.port = freePort();
    sc.maxClients = 3;
    sc.tls = &tls;
    sc.idleTimeoutMs = 2000;
    xHttpServer web(eth, sc);
    auth.attach(web);
    api.attach(web);
    web.get("/*", HttpStaticFiles::handler, &site);
    web.begin();
    std::thread daemon([&] { web.run(); });

    HttpsRedirect toHttps(sc.port);
    xHttpServer::Config plainCfg;
    plainCfg.port = freePort();
    plainCfg.maxClients = 1;
    xHttpServer plain(eth, plainCfg);
    plain.get("/*", HttpsRedirect::handler, &toHttps);
    plain.begin();
    std::thread plainDaemon([&] { plain.run(); });

    char caFile[] = "/tmp/plcca_XXXXXX", jar[] = "/tmp/plcjar_XXXXXX";
    int fd = mkstemp(caFile);
    if (fd < 0 || ::write(fd, testCaPem, std::strlen(testCaPem)) < 0) return 1;
    ::close(fd);
    fd = mkstemp(jar);
    ::close(fd);
    const std::string base = "https://127.0.0.1:" + std::to_string(sc.port);
    const std::string curl = std::string("curl -s --max-time 5 --cacert ") + caFile + " -c " + jar + " -b " + jar + " ";

    std::printf("reading, over HTTPS\n");
    std::string out;
    for (int i = 0; i < 100 && (out = run(curl + base + "/api/tags")).empty(); ++i) std::this_thread::sleep_for(milliseconds(20));
    check(has(out, "\"name\":\"Oven.Setpoint\",\"type\":\"REAL\",\"value\":60,\"writable\":true,\"webWritable\":true,\"min\":20,\"max\":250"),
          "the allow-listed setpoint, with its limits");
    check(has(out, "{\"name\":\"Safety.EStop\",\"type\":\"BOOL\",\"value\":false,\"writable\":true}"),
          "the E-stop: writable in the registry, not from the web");
    check(has(run(curl + base + "/"), "/api/login"), "the built-in page, with its login");
    check(has(run("curl -s -I --max-time 5 http://127.0.0.1:" + std::to_string(plainCfg.port) + "/"),
              "Location: " + base + "/"),
          "port 80 sends browsers to HTTPS");

    std::printf("writing\n");
    check(has(run(curl + "-H 'Content-Type: application/json' -d '{\"value\":100}' " + base + "/api/tags/Oven.Setpoint"),
              "login required") && setpoint == 60.0f,
          "no login: refused");
    std::string r = run(curl + "-H 'Content-Type: application/json' -d '{\"user\":\"viewer\",\"password\":\"view-password\"}' " +
                        base + "/api/login");
    char csrf[40] = "";
    HttpJson::string(r.data(), r.size(), "csrf", csrf, sizeof csrf);
    check(has(r, "\"role\":\"viewer\""), "the viewer logs in");
    check(has(run(curl + "-H 'Content-Type: application/json' -H 'X-CSRF-Token: " + csrf +
                  "' -d '{\"value\":100}' " + base + "/api/tags/Oven.Setpoint"), "not allowed") && setpoint == 60.0f,
          "the viewer may not write");
    r = run(curl + "-H 'Content-Type: application/json' -d '{\"user\":\"operator\",\"password\":\"op-password\"}' " + base +
            "/api/login");
    HttpJson::string(r.data(), r.size(), "csrf", csrf, sizeof csrf);
    check(has(r, "\"role\":\"operator\""), "the operator logs in");
    const std::string w = curl + "-H 'Content-Type: application/json' -H 'X-CSRF-Token: " + csrf + "' ";
    r = run(w + "-d '{\"value\":180}' " + base + "/api/tags/Oven.Setpoint");
    check(has(r, "\"value\":180") && setpoint == 180.0f, "the operator sets the oven to 180");
    check(has(run(w + "-d '{\"value\":400}' " + base + "/api/tags/Oven.Setpoint"), "outside the allowed range") &&
              setpoint == 180.0f,
          "400 is outside the allowed 20..250: refused");
    check(has(run(w + "-d '{\"value\":true}' " + base + "/api/tags/Safety.EStop"), "not writable from the web") && !estop,
          "the E-stop can't be touched from the web");
    check(has(run(w + "-d '{\"value\":5}' " + base + "/api/tags/Oven.Temp"), "not writable from the web"),
          "nor a reading");
    check(has(run(w + "-d '{\"value\":true}' " + base + "/api/tags/Oven.Pump"), "\"value\":true") && pump, "the pump on");
    std::this_thread::sleep_for(milliseconds(300));
    float t = 0;
    reg.readTag("Oven.Temp", &t, sizeof t);
    check(t > 25.0f, "and the control loop works toward the new setpoint");
    check(audit.get() == "operator set Oven.Setpoint 60 -> 180\noperator set Oven.Pump false -> true\n",
          "the audit log: who changed what, from what, to what");

    std::printf("the transport\n");
    check(has(run("curl -s --max-time 5 -H 'Content-Type: application/json' "
                  "-d '{\"user\":\"operator\",\"password\":\"op-password\"}' -X POST "
                  "http://127.0.0.1:" + std::to_string(plainCfg.port) + "/api/login"), "Method Not Allowed"),
          "no login over plain HTTP: port 80 only redirects");

    if (serveS > 0) {
        std::printf("serving %s for %d s, http on port %u (CA: %s; users operator/op-password, viewer/view-password)\n",
                    base.c_str(), serveS, plainCfg.port, caFile);
        std::this_thread::sleep_for(seconds(serveS));
        std::printf("audit:\n%s", audit.get().c_str());
    }

    web.stop();
    plain.stop();
    daemon.join();
    plainDaemon.join();
    quit = true;
    driver.join();
    stop = true;
    control.join();
    std::remove(caFile);
    std::remove(jar);
    std::printf("%s\n", g_failures ? "FAILED" : "all passed");
    return g_failures ? 1 : 0;
}
