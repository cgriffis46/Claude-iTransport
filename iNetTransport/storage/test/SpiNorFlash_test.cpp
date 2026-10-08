// Host test for SpiNorFlash, LittleFsNor and HttpLittleFsFiles, against
// the simulated chip in test/sim/SimSpiNor.h: the driver (IDs, sizes and
// 4-byte addressing, protection, pages, erase, busy and bus waits), then
// LittleFS on it serving pages and taking uploads through the real HTTP
// state machine, a remount, a power cut at every point of a file update,
// and readers and an uploader on several threads at once.
//
//   (L: a LittleFS v2.9 tree; cc -c -DLFS_NO_MALLOC -I$L $L/lfs.c $L/lfs_util.c)
//   (one line) g++ -std=c++14 -Wall -Wextra -pthread -DLFS_NO_MALLOC -Istorage/inc -Iinc -Ihttp/inc -Itest/sim
//       -I../iTransport/itransport/inc -I$L storage/test/SpiNorFlash_test.cpp storage/src/SpiNorFlash.cpp
//       storage/src/LittleFsNor.cpp storage/src/HttpLittleFsFiles.cpp http/src/*.cpp lfs.o lfs_util.o
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
#include "SpiNorFlash.h"
#include "LittleFsNor.h"
#include "HttpLittleFsFiles.h"
#include "HttpConnection.h"
#include "HttpLexer.h"
#include "SimSpiNor.h"

static int g_failures = 0;
static void check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failures;
}
static bool has(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

static uint32_t g_ms = 0;
static uint32_t clockFn(void*) { return g_ms; }
static int g_yields = 0;
static void yieldFn(void*) { ++g_yields; ++g_ms; }   // each wait moves the clock 1 ms

static SpiNorFlash::Config flashConfig() {
    SpiNorFlash::Config c;
    c.yield = yieldFn;
    c.now = clockFn;
    return c;
}

struct StdLock : iLock {
    std::mutex m;
    void lock() override { m.lock(); }
    void unlock() override { m.unlock(); }
};

// A request through the real lexer and state machine.
struct Out : HttpOutput {
    std::string data;
    bool write(const void* d, size_t n) override { data.append(static_cast<const char*>(d), n); return true; }
};
struct Sink : HttpTokenSink {
    std::deque<HttpToken> q;
    bool put(const HttpToken& t) override { q.push_back(t); return true; }
};
static std::string get(HttpRoutes& routes, const std::string& target) {
    uint8_t arena[1024];
    Out out;
    HttpConnection conn(routes, arena, sizeof arena, out);
    HttpLexer lx;
    Sink sink;
    const std::string req = "GET " + target + " HTTP/1.1\r\nHost: x\r\n\r\n";
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
    const size_t got = src.read(f, 0, reinterpret_cast<uint8_t*>(&s[0]), size);
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
    std::printf("the driver\n");
    {
        SimSpiNor chip(0xEF4018, 16u << 20);   // W25Q128
        chip.asleep = true;                    // left in deep power-down
        FakeNorBus bus(chip);
        SpiNorFlash flash(bus, flashConfig());
        check(flash.begin() && flash.size() == (16u << 20) && flash.jedecId() == 0xEF4018,
              "W25Q128, woken from deep power-down: 16 MB");
        const std::string data = pattern(300, 'a');
        check(flash.program(200, data.data(), data.size()), "300 bytes across a page boundary");
        std::string back(300, 0);
        check(flash.read(200, &back[0], 300) && back == data, "read back whole (no wrap within the page)");
        check(chip.mem[199] == 0xFF && chip.mem[500] == 0xFF, "nothing outside them touched");
        const uint8_t clear = 0x0F;
        flash.program(200, &clear, 1);
        check(chip.mem[200] == (uint8_t(data[0]) & 0x0F), "a program only clears bits");
        const uint8_t next = 0x11;
        flash.program(4096, &next, 1);   // the first byte of the next sector
        check(flash.erase(4000) && chip.mem[200] == 0xFF && chip.mem[4095] == 0xFF && chip.mem[4096] == 0x11,
              "erase: the 4 KB sector holding the address, and nothing else");
        check(chip.misuse == 0, "never a command while busy, nor a program without write enable");
        check(chip.lastOp == 0x05 && g_yields > 0, "it waited on the busy bit, yielding");
        check(!flash.read(16u << 20, &back[0], 1) && flash.error() == SpiNorFlash::Error::Range, "past the end: refused");

        bus.busyPolls = 3;
        bus.refuse = 4;
        check(flash.read(0, &back[0], 10), "a bus that's busy elsewhere, and slow transfers: waited for");
        bus.refuseForever = true;
        check(!flash.read(0, &back[0], 10) && flash.error() == SpiNorFlash::Error::Bus, "a bus never free: Bus, in time");
        bus.refuseForever = false;
        bus.busyPolls = 0;
        chip.busyForever = 1;
        check(!flash.erase(0) && flash.error() == SpiNorFlash::Error::Timeout, "a chip stuck busy: Timeout");
        chip.busyForever = 0;
    }
    {
        SimSpiNor chip(0xC22019, 32u << 20);   // MX25L25645: 32 MB
        FakeNorBus bus(chip);
        SpiNorFlash flash(bus, flashConfig());
        const uint8_t v[4] = {1, 2, 3, 4};
        uint8_t back[4] = {};
        check(flash.begin() && flash.size() == (32u << 20), "MX25L256: 32 MB");
        check(flash.program((32u << 20) - 4, v, 4) && chip.lastOp == 0x05 && flash.read((32u << 20) - 4, back, 4) &&
                  std::memcmp(back, v, 4) == 0 && chip.misuse == 0,
              "above 16 MB, with 4-byte addresses");
    }
    {
        SimSpiNor chip(0xBF2643, 8u << 20);    // SST26VF064B
        chip.sstLocked = true;
        FakeNorBus bus(chip);
        SpiNorFlash flash(bus, flashConfig());
        const uint8_t v = 0x5A;
        check(flash.begin() && flash.size() == (8u << 20) && !chip.sstLocked, "SST26VF064B: 8 MB, globally unlocked");
        check(flash.program(0, &v, 1) && chip.mem[0] == 0x5A, "and writable");
    }
    {
        SimSpiNor chip(0xEF4017, 8u << 20);
        chip.sr = 0x1C;                        // BP0..BP2: protected at power-up
        FakeNorBus bus(chip);
        SpiNorFlash flash(bus, flashConfig());
        check(flash.begin() && chip.sr == 0, "block protection found and cleared");
        chip.sr = 0x1C;
        chip.srLocked = true;
        check(!flash.begin() && flash.error() == SpiNorFlash::Error::WriteProtected, "a locked status register: WriteProtected");
        chip.sr = 0;
        chip.wpAll = true;
        const uint8_t v = 0;
        check(flash.begin() && !flash.program(0, &v, 1) && flash.error() == SpiNorFlash::Error::WriteProtected,
              "write enable that never takes: WriteProtected, not a silent no-op");
    }
    {
        SimSpiNor chip(0xEF4018, 16u << 20);
        FakeNorBus bus(chip);
        SpiNorFlash flash(bus, flashConfig());
        bus.stuckMiso = 0xFF;
        check(!flash.begin() && flash.error() == SpiNorFlash::Error::NoChip, "MISO high (no chip): NoChip");
        bus.stuckMiso = 0x00;
        check(!flash.begin() && flash.error() == SpiNorFlash::Error::NoChip, "MISO low: NoChip");
    }

    std::printf("LittleFS, serving and uploading\n");
    SimSpiNor chip(0xEF4016, 4u << 20);        // W25Q32: 4 MB
    FakeNorBus bus(chip);
    SpiNorFlash flash(bus, flashConfig());
    flash.begin();
    StdLock lock;
    LittleFsNor::Config lc;
    lc.lock = &lock;
    {
        LittleFsNor fs(flash, lc);
        check(fs.mount() && fs.blockCount() == 1024, "a blank chip formatted and mounted: 1024 blocks");
        HttpLittleFsFiles files(fs, "/www");
        check(files.writeFile("/index.html", reinterpret_cast<const uint8_t*>("<h1>flash</h1>"), 14), "writeFile()");
        check(readAll(files, "/index.html") == "<h1>flash</h1>", "read back");
        const std::string big = pattern(50000, 'x');
        size_t at = 0;
        bool ok = true;
        while (at < big.size() && ok) {   // as HttpFileAdmin's pieces arrive
            const size_t n = std::min<size_t>(1024, big.size() - at);
            ok = files.append("/js/app.js", at, reinterpret_cast<const uint8_t*>(big.data()) + at, n);
            at += n;
        }
        check(ok, "a 50 KB upload in 1 KB pieces, into a new folder");
        check(readAll(files, "/js/app.js") == "<none>", "not served before the commit");
        check(!files.append("/js/app.js", 100, reinterpret_cast<const uint8_t*>("x"), 1), "a piece at the wrong offset: refused");
        check(!files.commit("/js/app.js", big.size() + 1), "a commit of the wrong size: refused");
        check(files.commit("/js/app.js", big.size()) && readAll(files, "/js/app.js") == big, "committed: served, whole");
        check(!files.commit("/js/app.js", big.size()), "committed once only");

        std::vector<std::string> listed;
        files.list([](const char* p, size_t s, void* c) {
            static_cast<std::vector<std::string>*>(c)->push_back(std::string(p) + ":" + std::to_string(s));
        }, &listed);
        check(listed.size() == 2 && (listed[0] == "/index.html:14" || listed[1] == "/index.html:14") &&
                  (listed[0] == "/js/app.js:50000" || listed[1] == "/js/app.js:50000"),
              "list(): both, with sizes, under the root");
        files.append("/half.html", 0, reinterpret_cast<const uint8_t*>("abc"), 3);
        listed.clear();
        files.list([](const char* p, size_t, void* c) { static_cast<std::vector<std::string>*>(c)->push_back(p); }, &listed);
        check(listed.size() == 2, "an upload in progress isn't listed");
        uint64_t total = 0, free = 0;
        check(files.space(total, free) && total == (4u << 20) && free < total && free > total - 100000, "space()");

        HttpStaticFiles site(files);
        HttpRoutes routes;
        routes.on(HttpMethod::Get, "/*", HttpStaticFiles::handler, &site);
        check(body(get(routes, "/")) == "<h1>flash</h1>", "served over HTTP from flash");
        check(has(get(routes, "/.half.html.part"), "404"), "the hidden upload file isn't");
        check(files.remove("/index.html") && has(get(routes, "/"), "404"), "remove()");
        check(!files.remove("/js"), "a folder isn't a file to remove");
        files.writeFile("/index.html", reinterpret_cast<const uint8_t*>("v1"), 2);
        std::printf("  (all of that: %u sector erases, %u programs)\n", fs.erases(), fs.programs());
        fs.unmount();
    }
    {
        LittleFsNor fs(flash, lc);
        HttpLittleFsFiles files(fs, "/www");
        check(fs.mount() && readAll(files, "/index.html") == "v1" && readAll(files, "/js/app.js") == pattern(50000, 'x'),
              "mounted again: the files are there");
    }

    std::printf("power cuts\n");
    {
        // Replace index.html (v1 -> v2) with the power failing after k
        // flash operations, for every k up to where it completes.
        const std::string v2 = pattern(3000, 'v');
        int old = 0, fresh = 0, bad = 0;
        long k = 0;
        for (;; ++k) {
            {
                LittleFsNor fs(flash, lc);
                HttpLittleFsFiles files(fs, "/www");
                fs.mount();
                files.writeFile("/index.html", reinterpret_cast<const uint8_t*>("v1"), 2);   // back to v1
                chip.cutAfter = k;
                files.writeFile("/index.html", reinterpret_cast<const uint8_t*>(v2.data()), v2.size());
            }
            const bool completed = !chip.dead;
            chip.powerOn();
            flash.begin();
            LittleFsNor fs(flash, lc);
            HttpLittleFsFiles files(fs, "/www");
            const bool mounted = fs.mount();
            const std::string now = readAll(files, "/index.html");
            if (!mounted) ++bad;
            else if (now == "v1") ++old;
            else if (now == v2) ++fresh;
            else ++bad;
            if (completed) break;
        }
        char what[96];
        std::snprintf(what, sizeof what, "cut at each of %ld points: old file %d times, new %d, never corrupt", k, old, fresh);
        check(bad == 0 && old > 0 && fresh > 0, what);
    }

    std::printf("threads\n");
    {
        LittleFsNor fs(flash, lc);
        HttpLittleFsFiles files(fs, "/www");
        fs.mount();
        std::atomic<bool> stop{false};
        std::atomic<int> reads{0}, wrong{0};
        std::vector<std::thread> ts;
        const std::string big = pattern(50000, 'x');
        for (int t = 0; t < 3; ++t) {   // 3 readers + the uploader's read-back: the 4 slots
            ts.emplace_back([&] {
                while (!stop) {
                    const std::string s = readAll(files, "/js/app.js");
                    if (s == "<none>") continue;   // all slots busy: a 404, tried again
                    if (s != big) ++wrong;
                    ++reads;
                }
            });
        }
        int uploads = 0, i = 0;
        for (; i < 2000 && (i < 10 || reads < 30); ++i) {   // until the readers have had a good go
            const std::string page = pattern(2000 + i, char('a' + i % 20));
            if (files.writeFile("/page.html", reinterpret_cast<const uint8_t*>(page.data()), page.size()) &&
                readAll(files, "/page.html") == page) {
                ++uploads;
            }
        }
        stop = true;
        for (auto& t : ts) t.join();
        check(uploads == i && reads >= 30 && wrong == 0, "3 readers and an uploader at once: every read whole");
    }

    std::printf("replaced while being read\n");
    {
        LittleFsNor::Config small = lc;
        small.firstSector = 512;   // 32 blocks: LittleFS comes round to freed blocks quickly
        small.sectors = 32;
        LittleFsNor fs(flash, small);
        HttpLittleFsFiles files(fs, "/www");
        check(fs.format() && fs.mount(), "a 128 KB file system");
        size_t n = 0;
        files.writeFile("/a.html", reinterpret_cast<const uint8_t*>("aaaa"), 4);
        void* f = files.open("/a.html", n);
        check(files.busy("/a.html") && !files.busy("/b.html") && !files.remove("/a.html") &&
                  !files.writeFile("/a.html", reinterpret_cast<const uint8_t*>("b"), 1),
              "open for reading: busy(); remove and replace refused");
        files.close(f);
        check(!files.busy("/a.html") && files.writeFile("/a.html", reinterpret_cast<const uint8_t*>("b"), 1) &&
                  files.remove("/a.html"),
              "closed: both allowed");
        replaceWhileReading(files, "slow readers while it is replaced 60 times: every read one version, whole");
    }

    std::printf("%s\n", g_failures ? "FAILED" : "all passed");
    return g_failures ? 1 : 0;
}
