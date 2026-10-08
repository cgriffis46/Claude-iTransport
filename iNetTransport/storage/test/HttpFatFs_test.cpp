// Host test for HttpFatFsFiles: FatFs R0.15 (ST's, as CubeMX ships it)
// built for the PC with long file names, on a RAM disk standing in for an
// SD card. Serving through the real HTTP state machine, uploads in
// pieces and their commit, folders, listing, space, hidden files, a card
// pulled out, and readers with an uploader on several threads.
//
//   (F: FatFs's source/ folder; cc -c -Istorage/test/fatfs -I$F $F/ff.c $F/ffunicode.c)
//   (one line) g++ -std=c++14 -Wall -Wextra -pthread -Istorage/inc -Iinc -Ihttp/inc -Istorage/test/fatfs -I$F
//       storage/test/HttpFatFs_test.cpp storage/test/fatfs/RamDisk.cpp storage/src/HttpFatFsFiles.cpp
//       http/src/*.cpp ff.o ffunicode.o
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include "ff.h"
#include "HttpFatFsFiles.h"
#include "HttpConnection.h"
#include "HttpLexer.h"

extern bool ramDiskFail;

static int g_failures = 0;
static void check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failures;
}
static bool has(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

struct StdLock : iLock {
    std::mutex m;
    void lock() override { m.lock(); }
    void unlock() override { m.unlock(); }
};
struct Out : HttpOutput {
    std::string data;
    bool write(const void* d, size_t n) override { data.append(static_cast<const char*>(d), n); return true; }
};
struct Sink : HttpTokenSink {
    std::deque<HttpToken> q;
    bool put(const HttpToken& t) override { q.push_back(t); return true; }
};
static std::string get(HttpRoutes& routes, const std::string& target, const std::string& headers = "") {
    uint8_t arena[1024];
    Out out;
    HttpConnection conn(routes, arena, sizeof arena, out);
    HttpLexer lx;
    Sink sink;
    const std::string req = "GET " + target + " HTTP/1.1\r\nHost: x\r\n" + headers + "\r\n";
    lx.feed(reinterpret_cast<const uint8_t*>(req.data()), req.size(), sink);
    while (!sink.q.empty()) { conn.onToken(sink.q.front()); sink.q.pop_front(); }
    return out.data;
}
static std::string body(const std::string& r) { const size_t at = r.find("\r\n\r\n"); return at == std::string::npos ? "" : r.substr(at + 4); }
static std::string readAll(HttpFileSource& src, const char* path) {
    size_t size = 0;
    void* f = src.open(path, size);
    if (!f) return "<none>";
    std::string s(size, 0);
    const size_t got = size ? src.read(f, 0, reinterpret_cast<uint8_t*>(&s[0]), size) : 0;
    src.close(f);
    return got == size ? s : "<short>";
}
static std::string pattern(size_t n, char seed) {
    std::string s(n, 0);
    for (size_t i = 0; i < n; ++i) s[i] = static_cast<char>(seed + i * 7 + i / 300);
    return s;
}

// Clients reading a file slowly (a piece every millisecond), one after
// another, while it is replaced again and again: each must get one
// version, whole. commit() refuses while the file is open (busy()); the
// writer holds it closed to new readers and tries again, as HttpFileAdmin
// does, so it gets through although the file is nearly always open.
template <typename Files>
static void replaceWhileReading(Files& files, const char* what) {
    std::set<std::string> versions;
    std::mutex vm;
    auto version = [](int i) { return std::string(3000 + (i % 7) * 900, char('A' + i % 26)); };
    {
        const std::string v = version(0);
        versions.insert(v);
        files.writeFile("/live.html", reinterpret_cast<const uint8_t*>(v.data()), v.size());
    }
    std::atomic<bool> stop{false};
    std::atomic<int> reads{0}, torn{0}, refusedWhileOpen{0}, waits{0}, heldOut{0};
    std::vector<std::thread> ts;
    for (int t = 0; t < 3; ++t) {
        ts.emplace_back([&] {
            while (!stop) {
                size_t size = 0;
                void* f = files.open("/live.html", size);
                if (f) {
                    std::string s(size, 0);
                    size_t at = 0;
                    while (at < size) {
                        const size_t n = files.read(f, at, reinterpret_cast<uint8_t*>(&s[at]), std::min<size_t>(256, size - at));
                        if (n == 0) break;
                        at += n;
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    }
                    if (!files.busy("/live.html") || files.remove("/live.html")) ++refusedWhileOpen;   // must be busy, and kept
                    files.close(f);
                    std::lock_guard<std::mutex> g(vm);
                    if (at != size || !versions.count(s)) ++torn;
                    ++reads;
                } else {
                    ++heldOut;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(3));   // between fetches
            }
        });
    }
    int replaced = 0;
    for (int i = 1; i <= 60; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));   // an upload every so often
        const std::string v = version(i);
        {
            std::lock_guard<std::mutex> g(vm);
            versions.insert(v);
        }
        if (!files.append("/live.html", 0, reinterpret_cast<const uint8_t*>(v.data()), v.size())) break;
        bool done = files.commit("/live.html", v.size());
        if (!done) files.hold("/live.html", true);
        for (int tries = 0; tries < 5000 && !done; ++tries) {
            done = files.commit("/live.html", v.size());
            if (!done) {
                ++waits;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        files.hold("/live.html", false);
        if (done) ++replaced;
    }
    stop = true;
    for (auto& t : ts) t.join();
    std::printf("  (%d reads, %d held out; commits waited %d ms in all)\n", reads.load(), heldOut.load(), waits.load());
    check(replaced == 60 && reads > 20 && torn == 0 && refusedWhileOpen == 0 && waits > 0, what);
}

int main() {
    static FATFS fs;
    static BYTE work[FF_MAX_SS];
    std::printf("a card\n");
    check(f_mkfs("0:", nullptr, work, sizeof work) == FR_OK && f_mount(&fs, "0:", 1) == FR_OK, "formatted and mounted");

    StdLock lock;
    HttpFatFsFiles card("0:/www", &lock);
    std::vector<std::string> listed;
    check(card.list([](const char*, size_t, void*) {}, nullptr), "no folder yet: an empty list");
    check(card.writeFile("/index.html", reinterpret_cast<const uint8_t*>("<h1>card</h1>"), 13), "writeFile(), making /www");
    check(readAll(card, "/index.html") == "<h1>card</h1>", "read back");
    const std::string big = pattern(70000, 'q');
    size_t at = 0;
    bool ok = true;
    while (at < big.size() && ok) {
        const size_t n = std::min<size_t>(1024, big.size() - at);
        ok = card.append("/js/app.min.js.gz", at, reinterpret_cast<const uint8_t*>(big.data()) + at, n);
        at += n;
    }
    check(ok, "70 KB in 1 KB pieces, a long name in a new folder");
    check(readAll(card, "/js/app.min.js.gz") == "<none>", "not served before the commit");
    check(!card.append("/js/app.min.js.gz", 5, reinterpret_cast<const uint8_t*>("x"), 1), "the wrong offset: refused");
    check(!card.commit("/js/app.min.js.gz", 1), "the wrong size: refused");
    check(card.commit("/js/app.min.js.gz", big.size()) && readAll(card, "/js/app.min.js.gz") == big, "committed, whole");
    check(card.writeFile("/index.html", reinterpret_cast<const uint8_t*>("v2"), 2) && readAll(card, "/index.html") == "v2",
          "a file replaced");
    card.append("/half.txt", 0, reinterpret_cast<const uint8_t*>("x"), 1);
    card.list([](const char* p, size_t s, void* c) {
        static_cast<std::vector<std::string>*>(c)->push_back(std::string(p) + ":" + std::to_string(s));
    }, &listed);
    check(listed.size() == 2 && (listed[0] == "/index.html:2" || listed[1] == "/index.html:2") &&
              (listed[0] == "/js/app.min.js.gz:70000" || listed[1] == "/js/app.min.js.gz:70000"),
          "list(): the files, not the upload in progress");
    uint64_t total = 0, free = 0;
    check(card.space(total, free) && total > 7000000 && free < total && free > total - 200000, "space()");

    HttpStaticFiles site(card);
    HttpRoutes routes;
    routes.on(HttpMethod::Get, "/*", HttpStaticFiles::handler, &site);
    check(body(get(routes, "/")) == "v2", "served over HTTP from the card");
    const std::string gz = get(routes, "/js/app.min.js", "Accept-Encoding: gzip\r\n");
    check(has(gz, "Content-Encoding: gzip") && has(gz, "text/javascript") && body(gz) == big, "the .gz, for a browser taking gzip");
    check(has(get(routes, "/.half.txt.part"), "404"), "the hidden upload isn't served");
    check(card.remove("/index.html") && has(get(routes, "/"), "404") && !card.remove("/index.html"), "remove()");

    std::printf("the card pulled out\n");
    ramDiskFail = true;
    check(readAll(card, "/js/app.min.js.gz") != big && !card.writeFile("/x.txt", reinterpret_cast<const uint8_t*>("x"), 1),
          "reads and writes fail, without a crash");
    check(has(get(routes, "/js/app.min.js.gz"), "404") || has(get(routes, "/js/app.min.js.gz"), "200"), "the server still answers");
    ramDiskFail = false;
    f_mount(&fs, "0:", 1);
    check(readAll(card, "/js/app.min.js.gz") == big, "back, and remounted: there");

    {
        size_t n = 0;
        void* f[HttpFatFsFiles::kMaxOpen + 1];
        for (auto& x : f) x = card.open("/js/app.min.js.gz", n);
        bool full = f[HttpFatFsFiles::kMaxOpen] == nullptr;
        for (unsigned i = 0; i < HttpFatFsFiles::kMaxOpen; ++i) { full = full && f[i]; card.close(f[i]); }
        check(full && readAll(card, "/js/app.min.js.gz") == big, "kMaxOpen files open, the next refused; slots freed on close");
    }

    std::printf("threads\n");
    {
        std::atomic<bool> stop{false};
        std::atomic<int> reads{0}, wrong{0};
        std::vector<std::thread> ts;
        for (int t = 0; t < 3; ++t) {   // 3 readers + the uploader's read-back: the 4 slots
            ts.emplace_back([&] {
                while (!stop) {
                    const std::string s = readAll(card, "/js/app.min.js.gz");
                    if (s == "<none>") continue;
                    if (s != big) ++wrong;
                    ++reads;
                }
            });
        }
        int ups = 0, i = 0;
        for (; i < 2000 && (i < 10 || reads < 30); ++i) {   // until the readers have had a good go
            const std::string page = pattern(3000 + i, char('a' + i % 20));
            if (card.writeFile("/page.html", reinterpret_cast<const uint8_t*>(page.data()), page.size()) &&
                readAll(card, "/page.html") == page) {
                ++ups;
            }
        }
        stop = true;
        for (auto& t : ts) t.join();
        check(ups == i && reads >= 30 && wrong == 0, "3 readers and an uploader: every read whole");
    }

    std::printf("replaced while being read\n");
    {
        size_t n = 0;
        card.writeFile("/a.html", reinterpret_cast<const uint8_t*>("aaaa"), 4);
        void* f = card.open("/a.html", n);
        check(card.busy("/a.html") && !card.busy("/b.html") && !card.remove("/a.html") &&
                  !card.writeFile("/a.html", reinterpret_cast<const uint8_t*>("b"), 1),
              "open for reading: busy(); remove and replace refused");
        card.close(f);
        check(!card.busy("/a.html") && card.writeFile("/a.html", reinterpret_cast<const uint8_t*>("b"), 1) &&
                  card.remove("/a.html"),
              "closed: both allowed");
        replaceWhileReading(card, "slow readers while it is replaced 60 times: every read one version, whole");
    }

    std::printf("%s\n", g_failures ? "FAILED" : "all passed");
    return g_failures ? 1 : 0;
}
