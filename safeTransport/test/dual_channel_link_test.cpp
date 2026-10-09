// Host test for DualChannelLink: two simulated MCUs, each one channel
// of a safety relay (its own loopback, a SafeDevice, an output),
// joined by a simulated UART link in each direction.
//
// g++ -std=gnu++14 -fno-exceptions -fno-rtti -Wall -Wextra -I.. -I../../iTransport/itransport/inc dual_channel_link_test.cpp ../DualChannelLink.cpp ../SafeDevice.cpp -o dual_channel_link_test
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <vector>
#include "CrcS3.h"
#include "DualChannelLink.h"
#include "SafeDevice.h"
#include "SafeInput.h"
#include "SafeOutput.h"

static int g_checks = 0, g_failures = 0;
#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_failures; \
    std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

// ---- the simulated hardware ----

// One direction of the link: what one MCU's TX sends reaches the
// other's RX. Like a DMA UART, it sends from the caller's buffer
// while the transfer runs (no copy), so a sender that rebuilt the
// buffer too early would corrupt its own frame.
class SimUart : public iTransport {
public:
    unsigned bytesPerTick = 100;   // about 1 Mbaud with 1 ms ticks
    bool     connected = true;     // false: the wire is cut
    SimUart* peer = nullptr;       // whose receiver our TX reaches
    iTransportRxSink* sink = nullptr;
    int      corruptByte = -1;     // flip the byte at this index of the next frame
    bool     stuck = false;        // keep resending the last frame instead of new ones
    std::vector<uint8_t> lastFrame;

    bool write(const uint8_t* data, size_t len) override {
        if (pos_ < len_) return false;   // still sending
        data_ = data; len_ = len; pos_ = 0;
        if (!stuck) lastFrame.assign(data, data + len);
        return true;
    }
    void setRxSink(iTransportRxSink& s) override { sink = &s; }
    // The MCU resets: the transfer in progress stops, the receiver
    // forgets its sink.
    void reset() { data_ = nullptr; len_ = pos_ = 0; sink = nullptr; }

    void tick() {
        for (unsigned n = 0; n < bytesPerTick; ++n) {
            uint8_t b;
            if (stuck && !lastFrame.empty()) {
                b = lastFrame[stuckPos_++ % lastFrame.size()];
            } else {
                if (pos_ >= len_) return;
                b = data_[pos_];
                if (corruptByte == static_cast<int>(pos_)) { b ^= 0x10; corruptByte = -1; }
                ++pos_;
            }
            if (connected && peer && peer->sink) peer->sink->onByteReceived(b);
        }
        if (stuck) pos_ = len_;   // the sender sees its writes finish
    }

private:
    const uint8_t* data_ = nullptr;
    size_t len_ = 0, pos_ = 0, stuckPos_ = 0;
};

class TestChannel : public SafeInput {   // the MCU's own loopback, set by the test
public:
    void set(bool safe) { setSafe1State(safe); setSafe2State(safe); }
};

class TestOutput : public SafeOutput {
public:
    bool on = false;
    int  writes = 0;
    void setDesiredState(bool safe) override { on = safe; ++writes; setSafe1State(safe); setSafe2State(safe); }
    bool send() override { return true; }
};

struct Node {
    TestChannel     own;
    DualChannelLink link;
    TestOutput      out;
    SafeDevice      dev;
    Node(SimUart& uart, uint32_t id, const DualChannelLink::Config& cfg)
        : link(uart, own, id, cfg), dev(own, link, out) { own.set(true); }
};

// Two MCUs, A and B, each with a link UART.
struct Rig {
    SimUart uartA, uartB;   // uartA: A's link UART (A's TX -> B's RX)
    std::unique_ptr<Node> a, b;
    uint32_t idA, idB, now;
    DualChannelLink::Config cfg;

    Rig(uint32_t ida = 0x100, uint32_t idb = 0x200, uint32_t start = 0) : idA(ida), idB(idb), now(start) {
        uartA.peer = &uartB;
        uartB.peer = &uartA;
    }
    void bootA() { a.reset(); uartA.reset(); a.reset(new Node(uartA, idA, cfg)); }
    void bootB() { b.reset(); uartB.reset(); b.reset(new Node(uartB, idB, cfg)); }
    void step() {
        if (a) uartA.tick();
        if (b) uartB.tick();
        if (a) a->link.poll(now);
        if (b) b->link.poll(now);
        ++now;
    }
    void steps(int n) { while (n-- > 0) step(); }
    void resetBoth() { if (a) a->dev.reset(); if (b) b->dev.reset(); }
    bool bothOn() const { return a && b && a->out.on && b->out.on; }
};

static bool partnerSafe(const Node& n) { return n.link.GetSafe1State() && n.link.GetSafe2State(); }

// ---- tests ----

static void testBootTogether() {
    Rig r;
    r.bootA(); r.bootB();
    r.step();
    CHECK(!partnerSafe(*r.a));                        // never optimistically safe
    CHECK(!partnerSafe(*r.b));
    CHECK(r.a->link.role() == DualChannelLink::Role::Undecided);
    r.steps(5);
    CHECK(partnerSafe(*r.a));
    CHECK(partnerSafe(*r.b));
    CHECK(r.a->link.partnerHearsUs() && r.b->link.partnerHearsUs());
    CHECK(r.a->link.role() == DualChannelLink::Role::Primary);   // lower id
    CHECK(r.b->link.role() == DualChannelLink::Role::Secondary);
    CHECK(r.a->link.partnerId() == 0x200 && r.b->link.partnerId() == 0x100);
    CHECK(!r.a->out.on && !r.b->out.on);              // latched until reset
    r.resetBoth();
    CHECK(r.bothOn());
    r.steps(100);
    CHECK(r.bothOn());
    CHECK(r.a->link.stats().crcErrors == 0 && r.a->link.stats().sequenceErrors == 0);
    CHECK(r.a->link.stats().timeouts == 0);
}

static void testLowerIdIsPrimaryEitherWay() {
    Rig r(0x300, 0x200);
    r.bootA(); r.bootB();
    r.steps(5);
    CHECK(r.a->link.role() == DualChannelLink::Role::Secondary);
    CHECK(r.b->link.role() == DualChannelLink::Role::Primary);
}

// The old deadlock: neither side waits for the other before sending.
static void testOneBootsLate() {
    Rig r;
    r.bootA();
    r.steps(50);
    CHECK(r.a->link.stats().framesSent == 50);       // A kept sending alone
    CHECK(!partnerSafe(*r.a));
    CHECK(r.a->link.role() == DualChannelLink::Role::Undecided);
    r.a->dev.reset();
    CHECK(!r.a->out.on);                              // can't reset without a partner
    r.bootB();
    r.steps(5);
    CHECK(partnerSafe(*r.a) && partnerSafe(*r.b));
    r.resetBoth();
    CHECK(r.bothOn());
}

static void testPartnerLoopbackFails() {
    Rig r;
    r.bootA(); r.bootB();
    r.steps(5); r.resetBoth();
    CHECK(r.bothOn());
    r.b->own.set(false);                              // B's loopback breaks
    CHECK(!r.b->out.on);                              // B's own channel: at once
    r.step();                                         // B sends "loopback bad"
    r.step();                                         // A receives it
    CHECK(!partnerSafe(*r.a));
    CHECK(!r.a->out.on);                              // A trips too, within 2 ticks
    r.b->own.set(true);
    r.steps(3);
    CHECK(partnerSafe(*r.a));
    CHECK(!r.a->out.on && !r.b->out.on);              // still latched
    r.resetBoth();
    CHECK(r.bothOn());
}

// Lose and regain: a one-frame flicker of "loopback bad" inside a
// single poll() must still trip the partner.
static void testFlickerWithinOnePoll() {
    Rig r;
    r.uartB.bytesPerTick = 1000;
    r.bootA(); r.bootB();
    r.steps(5); r.resetBoth();
    CHECK(r.bothOn());
    // Two frames from B land in A's next poll: "bad", then "good".
    uint8_t f1[DualChannelLink::kFrameLen], f2[DualChannelLink::kFrameLen];
    const auto& st = r.a->link.stats();
    r.uartB.tick();                                   // B's frame still in flight lands first
    const uint32_t seqErrors = st.sequenceErrors;
    uint16_t seq = static_cast<uint16_t>(r.b->link.stats().framesSent);
    // B's next real frame will carry seq; inject the two ahead of it
    // with seq and seq+1, then stop B for this tick.
    DualChannelLink::encode(f1, seq, 0x200, DualChannelLink::kFlagHearingYou);
    DualChannelLink::encode(f2, static_cast<uint16_t>(seq + 1), 0x200,
                            DualChannelLink::kFlagHearingYou | DualChannelLink::kFlagLoopbackGood);
    for (uint8_t x : f1) r.a->link.onByteReceived(x);
    for (uint8_t x : f2) r.a->link.onByteReceived(x);
    r.a->link.poll(r.now);
    CHECK(st.sequenceErrors == seqErrors);            // both in sequence: no other fault
    CHECK(partnerSafe(*r.a));                         // the link looks fine again...
    CHECK(!r.a->out.on);                              // ...but A saw the drop and latched
}

static void testPartnerRebootsMidRun() {
    Rig r;
    r.bootA(); r.bootB();
    r.steps(10); r.resetBoth();
    CHECK(r.bothOn());
    r.bootB();                                        // B resets: its seq starts at 0 again
    CHECK(!r.b->out.on);
    r.steps(2);
    CHECK(!r.a->out.on);                              // A saw the jump back
    CHECK(r.a->link.stats().sequenceErrors >= 1);
    r.steps(3);
    CHECK(partnerSafe(*r.a) && partnerSafe(*r.b));
    r.resetBoth();
    CHECK(r.bothOn());
}

static void testWireCutOneWay() {
    Rig r;
    r.bootA(); r.bootB();
    r.steps(5); r.resetBoth();
    CHECK(r.bothOn());
    const uint32_t cutAt = r.now;
    r.uartB.connected = false;                        // B -> A broken; A -> B still fine
    uint32_t aOff = 0, bOff = 0;
    for (int i = 0; i < 10; ++i) {
        r.step();
        if (!aOff && !r.a->out.on) aOff = r.now - cutAt;
        if (!bOff && !r.b->out.on) bOff = r.now - cutAt;
    }
    CHECK(aOff >= 1 && aOff <= 4);                    // A: silence, timeout 3 ticks
    CHECK(bOff >= 1 && bOff <= 6);                    // B: A stops saying "hearing you"
    CHECK(r.a->link.role() == DualChannelLink::Role::Alone);
    CHECK(r.a->link.stats().timeouts == 1);
    CHECK(!r.b->link.partnerHearsUs());
    r.uartB.connected = true;
    r.steps(5);
    CHECK(partnerSafe(*r.a) && partnerSafe(*r.b));
    CHECK(r.a->link.role() == DualChannelLink::Role::Primary);
    CHECK(!r.a->out.on && !r.b->out.on);
    r.resetBoth();
    CHECK(r.bothOn());
}

static void testStuckLine() {
    Rig r;
    r.bootA(); r.bootB();
    r.steps(5); r.resetBoth();
    CHECK(r.bothOn());
    r.uartB.stuck = true;                             // B's line replays one frame forever
    r.steps(5);
    CHECK(!partnerSafe(*r.a));
    CHECK(!r.a->out.on);
    CHECK(r.a->link.stats().sequenceErrors >= 1);
}

static void testCorruptedByte() {
    Rig r;
    r.bootA(); r.bootB();
    r.steps(5); r.resetBoth();
    CHECK(r.bothOn());
    r.uartB.corruptByte = 5;                          // inside B's next frame's id
    r.step();
    CHECK(r.a->link.stats().crcErrors == 1);
    CHECK(!r.a->out.on);
    r.steps(3);
    CHECK(partnerSafe(*r.a));                         // resynchronised on the next frames
    CHECK(r.a->link.stats().crcErrors == 1);
    r.resetBoth();
    CHECK(r.bothOn());
}

static void testNoiseBeforePartner() {
    Rig r;
    r.bootA();
    const uint8_t noise[] = {0x00, 0xA5, 0x5A, 0x01, 0xA5, 0xA5, 0x5A, 0xFF, 0x12, 0x5A, 0xA5,
                             0x5A, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x03, 0x77, 0x88, 0xA5};
    for (uint8_t x : noise) r.a->link.onByteReceived(x);
    r.step();
    CHECK(!partnerSafe(*r.a));
    r.bootB();
    r.steps(5);
    CHECK(partnerSafe(*r.a));
}

static void testLinkWiredToItself() {
    Rig r;
    r.uartA.peer = &r.uartA;                          // A's TX looped to A's RX
    r.bootA();
    r.steps(10);
    CHECK(r.a->link.stats().ownIdFrames >= 5);
    CHECK(!partnerSafe(*r.a));
    CHECK(r.a->link.role() == DualChannelLink::Role::Undecided);
}

static void testUnknownFlagsRejected() {
    Rig r;
    r.bootA();
    uint8_t f[DualChannelLink::kFrameLen];
    for (uint16_t s = 0; s < 4; ++s) {
        DualChannelLink::encode(f, s, 0x200, 0x80 | DualChannelLink::kFlagLoopbackGood |
                                             DualChannelLink::kFlagHearingYou);
        for (uint8_t x : f) r.a->link.onByteReceived(x);
        r.step();
    }
    CHECK(r.a->link.stats().badFlagFrames == 4);
    CHECK(!partnerSafe(*r.a));
}

static void testReplacedBoard() {
    Rig r;
    r.bootA(); r.bootB();
    r.steps(5); r.resetBoth();
    r.idB = 0x050;                                    // a new board with a lower id
    r.bootB();
    r.steps(5);
    CHECK(r.a->link.partnerId() == 0x050);
    CHECK(r.a->link.role() == DualChannelLink::Role::Secondary);
    CHECK(r.b->link.role() == DualChannelLink::Role::Primary);
    CHECK(!r.a->out.on);
}

// A slow UART: write() refuses while a frame is still going out, and
// the frame being sent must not be rebuilt under it.
static void testBusyWriter() {
    Rig r;
    r.cfg.timeoutTicks = 5;
    r.uartA.bytesPerTick = 4;                         // a frame takes 3 ticks
    r.uartB.bytesPerTick = 4;
    r.bootA(); r.bootB();
    r.steps(20); r.resetBoth();
    r.steps(200);
    CHECK(r.bothOn());
    CHECK(r.a->link.stats().crcErrors == 0 && r.b->link.stats().crcErrors == 0);
    CHECK(r.a->link.stats().sequenceErrors == 0);
    CHECK(r.a->link.stats().framesSent < 100);        // refused writes were not counted
}

static void testTickRollover() {
    Rig r(0x100, 0x200, 0xFFFFFFF0u);
    r.bootA(); r.bootB();
    r.steps(5); r.resetBoth();
    r.steps(40);                                      // across the wrap
    CHECK(r.bothOn());
    CHECK(r.a->link.stats().timeouts == 0 && r.b->link.stats().timeouts == 0);
}

static void testRxOverflow() {
    Rig r;
    r.bootA(); r.bootB();
    r.steps(5); r.resetBoth();
    CHECK(r.bothOn());
    for (int i = 0; i < 100; ++i) r.a->link.onByteReceived(0x00);   // more than the ring holds
    r.a->link.poll(r.now);
    CHECK(r.a->link.stats().rxOverflows > 0);
    CHECK(!r.a->out.on);
    r.steps(5);
    CHECK(partnerSafe(*r.a));
}

static void testFrameFormat() {
    uint8_t f[DualChannelLink::kFrameLen];
    DualChannelLink::encode(f, 0x1234, 0xA1B2C3D4u, 0x03);
    const uint8_t head[] = {0xA5, 0x5A, 0x34, 0x12, 0xD4, 0xC3, 0xB2, 0xA1, 0x03};
    CHECK(std::memcmp(f, head, sizeof head) == 0);
    const uint16_t crc = CrcS3::compute(f + 2, 7, 0xFFFF);
    CHECK(f[9] == (crc & 0xFF) && f[10] == (crc >> 8));
}

int main() {
    testFrameFormat();
    testBootTogether();
    testLowerIdIsPrimaryEitherWay();
    testOneBootsLate();
    testPartnerLoopbackFails();
    testFlickerWithinOnePoll();
    testPartnerRebootsMidRun();
    testWireCutOneWay();
    testStuckLine();
    testCorruptedByte();
    testNoiseBeforePartner();
    testLinkWiredToItself();
    testUnknownFlagsRejected();
    testReplacedBoard();
    testBusyWriter();
    testTickRollover();
    testRxOverflow();
    std::printf("dual_channel_link_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
