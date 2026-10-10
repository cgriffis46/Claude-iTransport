// Host test for the debug log (DebugLog.h) and the bus hooks, built at
// ITRANSPORT_DEBUG=3 whatever the rest of the build uses, and with
// debug_log_off.cpp built at 0 to check the macros vanish there.
//
//   g++ -std=c++17 -Wall -Wextra -DITRANSPORT_DEBUG=3 -I../inc debug_log_test.cpp debug_log_off.cpp
//       ../src/DebugLog.cpp ../src/BusTransport.cpp ../src/I2CTransport.cpp -o debug_log_test
// (debug_log_off.cpp sets its own level to 0 before including DebugLog.h.)
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "DebugLog.h"
#include "I2CTransport.h"

static int g_checks = 0, g_failures = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { ++g_failures; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

int offSideEffects();   // debug_log_off.cpp

// ---- the debug UART: sends from the caller's buffer, a few bytes a
// tick, refusing a new write while one is going out (as HAL_UART_Transmit_IT does)
struct SlowUart : iTransport {
    std::string wire;
    const uint8_t* data = nullptr;
    size_t len = 0, pos = 0;
    unsigned perTick = 1000;
    int refused = 0;
    bool write(const uint8_t* d, size_t n) override {
        if (pos < len) { ++refused; return false; }
        data = d; len = n; pos = 0;
        return true;
    }
    void setRxSink(iTransportRxSink&) override {}
    void tick() { for (unsigned i = 0; i < perTick && pos < len; ++i) wire += (char)data[pos++]; }
};

// ---- the port ----
static uint32_t g_ms = 0;
static int g_locks = 0, g_depth = 0, g_maxDepth = 0;
static std::vector<std::string> g_pins;
static uint32_t nowMs() { return g_ms; }
static uint32_t lock() { ++g_locks; ++g_depth; if (g_depth > g_maxDepth) g_maxDepth = g_depth; return 7; }
static void unlock(uint32_t saved) { --g_depth; if (saved != 7) g_depth = -100; }
static void pin(uint8_t id, bool level) { g_pins.push_back(std::to_string(id) + (level ? "+" : "-")); }
static dbg::Port port() { dbg::Port p; p.nowMs = &nowMs; p.lock = &lock; p.unlock = &unlock; p.pin = &pin; return p; }

static std::vector<std::string> lines(const std::string& w) {
    std::vector<std::string> out;
    size_t p = 0;
    for (;;) {
        size_t e = w.find("\r\n", p);
        if (e == std::string::npos) break;
        out.push_back(w.substr(p, e - p));
        p = e + 2;
    }
    return out;
}

static void drain(SlowUart& u) { for (int i = 0; i < 1000; ++i) { dbg::poll(); u.tick(); } }

static void testFormat() {
    dbg::reset();
    SlowUart u;
    dbg::begin(&u, port());
    g_ms = 53187;
    DBG_EVENT("mtk3339", "st", 1, 2);
    g_ms = 53188;
    DBG_FAULT("bus", "xfer-fail", -5);
    DBG_TRACE("ublox", "cfg-done");
    dbg::event("x", "many", {1, 2, 3, 4, 5, 6, 7, 8});   // only the first kMaxValues
    dbg::event("x", "big", {2147483647, -2147483647 - 1});
    drain(u);
    const std::vector<std::string> l = lines(u.wire);
    CHECK(l.size() == 5);
    CHECK(l[0] == "1 53187 mtk3339 st 1 2");
    CHECK(l[1] == "2 53188 bus xfer-fail -5");
    CHECK(l[2] == "3 53188 ublox cfg-done");
    CHECK(l[3] == "4 53188 x many 1 2 3 4 5 6");
    CHECK(l[4] == "5 53188 x big 2147483647 -2147483648");
    CHECK(dbg::stats().lines == 5 && dbg::stats().dropped == 0 && dbg::stats().bytesSent == u.wire.size());
    CHECK(g_depth == 0 && g_locks > 0);   // every lock undone, the saved value given back
    // Each line goes into the ring under the lock (once), so an interrupt
    // logging at the same moment can't interleave with it.
    const int before = g_locks;
    g_maxDepth = 0;
    DBG_EVENT("x", "locked");
    CHECK(g_locks == before + 1 && g_maxDepth == 1 && g_depth == 0);
}

// More than the ring holds before poll(): the overflow is dropped and
// counted, the seq numbers show the gap, and nothing that was kept is
// damaged.
static void testOverflow() {
    dbg::reset();
    SlowUart u;
    dbg::begin(&u, port());
    g_ms = 1;
    for (int i = 0; i < 200; ++i) DBG_EVENT("tag", "event", i);
    drain(u);
    const dbg::Stats st = dbg::stats();
    const std::vector<std::string> l = lines(u.wire);
    CHECK(st.lines == 200 && st.dropped > 0 && l.size() == 200 - st.dropped);
    CHECK(l.front() == "1 1 tag event 0");
    // Lines kept are whole and in order.
    int last = 0;
    bool ok = true;
    for (const std::string& s : l) {
        int seq = 0, ms = 0, v = -1;
        char tag[16], what[16];
        if (std::sscanf(s.c_str(), "%d %d %15s %15s %d", &seq, &ms, tag, what, &v) != 5 || seq <= last || v != seq - 1) ok = false;
        last = seq;
    }
    CHECK(ok);
    // After draining, there is room again.
    DBG_EVENT("tag", "after");
    drain(u);
    CHECK(lines(u.wire).back() == "201 1 tag after");
}

// The UART is slow: poll() must hand it whole buffers, never overwrite
// one it is still sending, and keep the order.
static void testSlowUart() {
    dbg::reset();
    SlowUart u;
    u.perTick = 3;
    g_ms = 0;
    dbg::begin(&u, port());
    for (int i = 0; i < 30; ++i) {
        DBG_EVENT("slow", "line", i, i * 1000);
        dbg::poll();
        u.tick();
    }
    drain(u);
    const std::vector<std::string> l = lines(u.wire);
    CHECK(l.size() == 30 && dbg::stats().dropped == 0);
    bool ok = true;
    for (int i = 0; i < 30 && i < (int)l.size(); ++i) {
        if (l[i] != std::to_string(i + 1) + " 0 slow line " + std::to_string(i) + " " + std::to_string(i * 1000)) ok = false;
    }
    CHECK(ok);
    CHECK(u.refused > 0);   // it really was busy at times
}

// No UART, or no port at all: nothing breaks.
static void testNoUart() {
    dbg::reset();
    DBG_EVENT("a", "b", 1);
    dbg::poll();
    CHECK(dbg::stats().lines == 0);
    g_pins.clear();
    dbg::Port p = port();
    dbg::begin(nullptr, p);   // pins only
    DBG_PULSE(dbg::kPinState);
    CHECK(g_pins.size() == 2 && g_pins[0] == "2+" && g_pins[1] == "2-");
    dbg::Port none = {nullptr, nullptr, nullptr, nullptr};
    SlowUart u;
    dbg::begin(&u, none);   // no clock, no lock, no pins
    DBG_FAULT("a", "b");
    DBG_PIN(dbg::kPinFault, true);
    drain(u);
    CHECK(u.wire == "1 0 a b\r\n");
}

// ---- the bus hooks ----
static int g_bus;
class FakeI2C : public I2CTransport {
public:
    FakeI2C() : I2CTransport(&g_bus, 0x38, nullptr) {}
    void complete(bool failed) { BusTransport::onTransferComplete(&g_bus, failed); }
protected:
    bool halMemWrite(uint8_t, uint8_t*, uint16_t) override { return true; }
    bool halMemRead(uint8_t, uint8_t*, uint16_t) override { return true; }
    bool halMasterTransmit(uint8_t*, uint16_t) override { return true; }
    bool halMasterReceive(uint8_t*, uint16_t) override { return true; }
    bool halIsDeviceReady(uint32_t, uint32_t) override { return true; }
};

static void testBusHooks() {
    dbg::reset();
    SlowUart u;
    dbg::begin(&u, port());
    g_pins.clear();
    FakeI2C bus;
    uint8_t buf[2];
    CHECK(bus.readRegs(0x10, buf, 2));
    CHECK(bus.isBusy());
    bus.complete(false);   // the interrupt
    CHECK(!bus.isBusy());
    CHECK(bus.readRegs(0x10, buf, 2));
    bus.complete(true);
    CHECK(!bus.isBusy() && bus.lastOpFailed());
    int other;
    BusTransport::onTransferComplete(&other, false);   // nobody's bus
    drain(u);
    const std::vector<std::string> l = lines(u.wire);
    // start, done, start, xfer-fail (in the interrupt), done, irq-unknown
    std::vector<std::string> what;
    for (const std::string& s : l) {
        char tag[16], w[24];
        int seq, ms;
        if (std::sscanf(s.c_str(), "%d %d %15s %23s", &seq, &ms, tag, w) == 4) what.push_back(std::string(tag) + " " + w);
    }
    const std::vector<std::string> want = {"bus start", "bus done", "bus start", "bus xfer-fail", "bus done", "bus irq-unknown"};
    CHECK(what == want);
    CHECK(l.size() >= 5 && l[4].substr(l[4].size() - 4) == " 0 1");   // done, failed
    // Pins: transfer high, interrupt pulse, transfer low; twice; one more pulse.
    const std::vector<std::string> pins = {"1+", "0+", "0-", "1-", "1+", "0+", "0-", "1-", "0+", "0-"};
    CHECK(g_pins == pins);
}

int main() {
    testFormat();
    testOverflow();
    testSlowUart();
    testNoUart();
    testBusHooks();
    CHECK(offSideEffects() == 0);
    std::printf("debug_log_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
