#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include "SafeInput.h"
#include "iTransport.h"

// The cross link between the two MCUs of a two-channel safety relay.
// Each MCU is one channel: it checks its own UART loopback, and sends
// its partner a heartbeat over a second UART (TX to the partner's RX,
// both ways). This class is that second UART's protocol, and on each
// MCU it is a SafeInput standing for the PARTNER's channel:
//
//   UartLoopbackChannelSafeInput ownLoop(loopUart, ...);   // my channel
//   DualChannelLink              partner(linkUart, ownLoop, myId);
//   SafeDevice                   relay(ownLoop, partner, output, &resetButton);
//
// SafeDevice then does what it already does: latched unsafe at boot,
// unsafe at once when either channel drops or they disagree, safe
// again only on reset() while both agree.
//
// Why this cannot deadlock at startup: every heartbeat is sent on a
// timer from the first poll(), whatever the partner is doing, and
// says only what this MCU measured itself (its own loopback). Nothing
// sent depends on the partner. A latch clears on "my loopback is good
// and my partner is talking and says its loopback is good", which
// never waits on the partner's latch, so both latches can clear.
//
// Heartbeat frame, 11 bytes, little-endian:
//   0xA5 0x5A        start (resynchronises after noise or a lost byte)
//   seq   uint16     +1 every frame, from 0 at boot
//   id    uint32     the sender's unique ID (e.g. part of the STM32 UID)
//   flags uint8      bit 0: my loopback is good
//                    bit 1: I am hearing you (good frames, in sequence)
//                    bits 2-7: zero (a frame with any set is rejected)
//   crc   uint16     CRC-S3 from 0xFFFF over seq..flags
//
// The partner's channel is safe only while all of these hold:
//   - frames keep arriving: the last good one is under timeoutTicks old
//   - each frame's seq is exactly the last one + 1: a repeat (a stuck
//     or replaying line), a gap (a lost or corrupted frame) or a jump
//     back (the partner reset) makes it unsafe, and it then needs
//     framesToTrust frames in sequence again
//   - the frame's id is not our own (a link wired back to itself)
//   - the partner says its loopback is good
//   - the partner says it is hearing us. So a link broken one way
//     turns both MCUs' channels unsafe, not just the deaf one's. This
//     can't deadlock either: "hearing you" depends only on receiving
//     frames, and both sides always send.
//
// Primary: once both have heard each other, the lower id is Primary.
// The role is for the jobs that are not safety (reporting, talking to
// the zone controller); safety stays symmetric, each MCU driving its
// own output channel. A partner that goes silent after being heard
// leaves this MCU Alone: it may carry on reporting, but its channel
// is unsafe until the partner is back and a reset is done.
//
// Threads: onByteReceived() runs in the UART's interrupt and only
// puts the byte into a single-producer, single-consumer ring (atomic
// indices, load and store only, so it also suits a Cortex-M0).
// Everything else, including the Safe callbacks, runs in poll() in the
// caller's task. A full ring counts as a lost frame.
//
// Timing: everything is in the caller's ticks. To meet a 4.17 ms
// response (1/4 cycle at 60 Hz) with 1 ms ticks, poll() every tick,
// send every tick (periodTicks = 1) and time out after 3. An 11 byte
// frame takes 110 us at 1 Mbaud and about 0.95 ms at 115200, so run
// the link at 1 Mbaud or so. These numbers are not checked on a chip.
// Not a TransportSafeInput: that attaches the receive hook in its own
// constructor, before this class's ring exists, and a byte can arrive
// as soon as it is attached. This attaches it last.
class DualChannelLink : public SafeInput, public iTransportRxSink {
public:
    static constexpr size_t  kFrameLen = 11;
    static constexpr uint8_t kStart0 = 0xA5;
    static constexpr uint8_t kStart1 = 0x5A;
    static constexpr uint8_t kFlagLoopbackGood = 0x01;
    static constexpr uint8_t kFlagHearingYou   = 0x02;
    static constexpr size_t  kRxRingSize = 64;   // power of two

    struct Config {
        uint32_t periodTicks   = 1;   // one heartbeat every this many ticks
        uint32_t timeoutTicks  = 3;   // partner unsafe once silent this long
        uint32_t framesToTrust = 2;   // frames in sequence before it counts
    };

    enum class Role {
        Undecided,   // the partner has not been heard yet
        Primary,     // heard; our id is the lower
        Secondary,   // heard; the partner's id is the lower
        Alone        // was heard, has gone silent
    };

    // Counters, for diagnostics and tests.
    struct Stats {
        uint32_t framesSent = 0;
        uint32_t framesGood = 0;
        uint32_t crcErrors = 0;
        uint32_t sequenceErrors = 0;   // repeats, gaps and jumps back
        uint32_t ownIdFrames = 0;
        uint32_t badFlagFrames = 0;
        uint32_t rxOverflows = 0;
        uint32_t timeouts = 0;
    };

    // link and ownChannel must outlive this. ownChannel is this MCU's
    // own loopback; its state is what each heartbeat reports. ownId
    // must differ from the partner's.
    DualChannelLink(iTransport& link, const Safe& ownChannel, uint32_t ownId,
                    const Config& config);
    DualChannelLink(iTransport& link, const Safe& ownChannel, uint32_t ownId)
        : DualChannelLink(link, ownChannel, ownId, Config()) {}

    // Interrupt context: queues the byte for poll().
    void onByteReceived(uint8_t byte) override;

    // Call every tick from the safety task: reads queued bytes, judges
    // the partner, sends a heartbeat when one is due.
    void poll(uint32_t nowTicks);

    Role         role() const { return role_; }
    bool         isPrimary() const { return role_ == Role::Primary; }
    bool         partnerHealthy() const { return healthy_; }
    bool         partnerHearsUs() const { return healthy_ && partnerHearsUs_; }
    bool         partnerLoopbackGood() const { return healthy_ && partnerLoopGood_; }
    uint32_t     partnerId() const { return partnerId_; }
    const Stats& stats() const { return stats_; }

    // Builds a heartbeat into out (kFrameLen bytes). Public for tests
    // and for anything that needs to look at the wire format.
    static void encode(uint8_t* out, uint16_t seq, uint32_t id, uint8_t flags);

private:
    void readQueuedBytes(uint32_t nowTicks);
    void parseByte(uint8_t byte, uint32_t nowTicks);
    void tryFrame(uint32_t nowTicks);
    void acceptFrame(uint16_t seq, uint32_t id, uint8_t flags, uint32_t nowTicks);
    void fault();
    void sendHeartbeat(uint32_t nowTicks);
    void report();

    iTransport&  link_;
    const Safe&  ownChannel_;
    uint32_t     ownId_;
    Config       config_;

    // Interrupt -> task ring.
    uint8_t               rxRing_[kRxRingSize] = {0};
    std::atomic<uint32_t> rxHead_{0};   // written by the interrupt only
    std::atomic<uint32_t> rxTail_{0};   // written by poll() only
    std::atomic<uint32_t> rxOverflows_{0};   // written by the interrupt only
    uint32_t              rxOverflowsSeen_ = 0;

    // Frame being assembled (task side).
    uint8_t frame_[kFrameLen] = {0};
    size_t  frameLen_ = 0;

    // Partner.
    bool     heardAny_ = false;    // ever had a good frame
    bool     haveSeq_ = false;     // lastSeq_ is a baseline to check against
    uint16_t lastSeq_ = 0;
    uint32_t inSequence_ = 0;      // good frames in a row since the last fault
    uint32_t lastGoodTicks_ = 0;
    uint32_t partnerId_ = 0;
    bool     partnerLoopGood_ = false;
    bool     partnerHearsUs_ = false;
    bool     healthy_ = false;
    Role     role_ = Role::Undecided;

    // Sending: two buffers, because iTransport::write() may send from
    // the buffer after it returns; a new frame is never built into the
    // one last handed to write().
    uint8_t  tx_[2][kFrameLen] = {{0}};
    int      txNext_ = 0;
    uint16_t txSeq_ = 0;
    bool     sentAny_ = false;
    uint32_t lastSendTicks_ = 0;

    Stats stats_;
};
