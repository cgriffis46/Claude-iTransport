// End-to-end host test: the PLC tag database served by xHttpServer over
// SocketNetDevice on the operating system's sockets, fetched with curl
// and checked with Python's JSON parser, while a "control loop" thread
// keeps changing a tag. Also the built-in page, a page from a folder
// overriding it, and that writes are refused.
//
// Built by CMake with the iNetTransport sources it needs (HOST builds).
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include "NetSockets.h"
#include "SocketNetDevice.h"
#include "xEthernet.h"
#include "xHttpServer.h"
#include "HttpFiles.h"
#include "PlcTagWebApi.h"
#include "PlcWebDefaultPage.h"

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

int main() {
    // The PLC: tags, and a control loop changing one of them.
    PlcTagRegistry reg;
    float temp = 21.5f;
    int32_t count = 0;
    bool running = true;
    reg.registerTag("Line1.Temp", temp);
    reg.registerTag("Line1.Count", count, false);
    reg.registerTag("Line1.Running", running);
    std::atomic<bool> stop{false};
    std::thread control([&] {
        while (!stop) {
            int32_t c = 0;
            reg.readTag("Line1.Count", &c, sizeof c);
            ++c;
            count = c;   // as a control program would: the registry only points at it
            std::this_thread::sleep_for(milliseconds(5));
        }
    });

    // The network: the host's sockets, as lwIP's would be on the F207.
    SocketNetDevice dev;
    xEthernet eth(dev);
    NetConfig net;
    net.ip = IpAddress(127, 0, 0, 1);
    net.subnet = IpAddress(255, 0, 0, 0);
    eth.begin(net);
    std::atomic<bool> quit{false};
    std::thread driver([&] { while (!quit) eth.service(20); });
    eth.waitAddress(2000);

    // The web server: the JSON API, then the user's files, then the built-in page.
    char dir[] = "/tmp/plcwwwXXXXXX";
    if (mkdtemp(dir) == nullptr) return 1;
    HttpStdioFiles disk(dir);
    HttpMemoryFiles builtIn(plcWebDefaultFiles, plcWebDefaultFileCount);
    HttpStaticFiles site(disk, &builtIn);
    PlcTagWebApi api(reg);
    xHttpServer::Config wcfg;
    wcfg.port = freePort();
    xHttpServer web(eth, wcfg);
    api.attach(web);
    web.get("/*", HttpStaticFiles::handler, &site);
    web.begin();
    std::thread daemon([&] { web.run(); });
    const std::string base = "http://127.0.0.1:" + std::to_string(wcfg.port);

    std::printf("the PLC on the web\n");
    std::string json;
    for (int i = 0; i < 100 && json.empty(); ++i) {
        json = run("curl -s --max-time 2 " + base + "/api/tags");
        if (json.empty()) std::this_thread::sleep_for(milliseconds(20));
    }
    check(has(json, "\"name\":\"Line1.Temp\",\"type\":\"REAL\",\"value\":21.5,\"writable\":true"), "curl /api/tags");
    const std::string py =
        "python3 -c \"import json,sys; t=json.load(sys.stdin)['tags']; "
        "print(len(t), [x['name'] for x in t], t[1]['value'] > 0, t[2]['value'])\"";
    const std::string parsed = run("curl -s --max-time 2 " + base + "/api/tags | " + py);
    check(has(parsed, "3 ['Line1.Temp', 'Line1.Count', 'Line1.Running'] True True"), "valid JSON, to Python's parser");

    auto countNow = [&] {
        const std::string one = run("curl -s --max-time 2 " + base + "/api/tags/Line1.Count");
        const size_t at = one.find("\"value\":");
        return at == std::string::npos ? -1L : std::atol(one.c_str() + at + 8);
    };
    const long c1 = countNow();
    std::this_thread::sleep_for(milliseconds(100));
    const long c2 = countNow();
    check(c1 > 0 && c2 > c1, "live values: the control loop's count rises between requests");

    check(run("curl -s --max-time 2 '" + base + "/api/tags?names=Line1.Running,Nope'") ==
              "{\"tags\":[{\"name\":\"Line1.Running\",\"type\":\"BOOL\",\"value\":true,\"writable\":true},"
              "{\"name\":\"Nope\",\"error\":\"no such tag\"}]}",
          "?names=");
    check(run("curl -s -o /dev/null -w '%{http_code}' --max-time 2 -X POST --data 'value=1' " + base +
              "/api/tags/Line1.Temp") == "405",
          "POST to a tag: 405, read-only");
    check(temp == 21.5f, "and the tag is unchanged");

    check(has(run("curl -s --max-time 2 " + base + "/"), "fetch('/api/tags'"), "/ : the built-in page");
    FILE* f = std::fopen((std::string(dir) + "/index.html").c_str(), "w");
    std::fputs("<h1>Line 1</h1>", f);
    std::fclose(f);
    check(run("curl -s --max-time 2 " + base + "/") == "<h1>Line 1</h1>", "a page put in the folder replaces it, no firmware change");

    web.stop();
    daemon.join();
    quit = true;
    driver.join();
    stop = true;
    control.join();
    run(std::string("rm -rf ") + dir);
    std::printf("%s\n", g_failures ? "FAILED" : "all passed");
    return g_failures ? 1 : 0;
}
