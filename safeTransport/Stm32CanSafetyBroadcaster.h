#pragma once
#include <cstdint>
#include <cstddef>
#include "Safe.h"
#include "iTransport.h"

// Broadcasts a Safe source's Safe1/Safe2 state over CAN, split
// across two independent 6-byte frames — one per channel. This is
// the concrete frame format worked out over several turns:
//
//   byte 0-1: this channel's 16-bit half of the 32-bit identity code
//             (channel 1 carries the LOWER half, channel 2 the
//             UPPER — the safe zone controller only reconstructs a
//             valid, complete identity if BOTH frames arrive
//             correctly, tying identity verification to channel-
//             health verification directly, rather than treating
//             them as separate concerns)
//   byte 2:   this channel's own safety data (0x01 safe, 0x00 unsafe)
//   byte 3:   bitwise-inverted copy of byte 2 — CANopen Safety's
//             redundant-encoding check, catching corruption patterns
//             a CRC alone might miss
//   byte 4:   a SHARED sequence counter, incremented once per
//             broadcast() call and used by BOTH channels' frames for
//             that round — deliberately shared, not independent per
//             channel: this lets a receiver verify both frames came
//             from the SAME round, not just that each is
//             individually fresh, catching a "mismatched round"
//             replay a per-channel counter wouldn't
//   byte 5:   CRC-8 (see Crc8.h) over bytes 0-4
//
// transport1/transport2 are two SEPARATE CAN transports (e.g. two
// Stm32HalCanTransport instances, each already configured with its
// own distinct, fixed txId) — using distinct arbitration IDs per
// channel rather than an extra "which channel" byte in the payload,
// which would have left only 1 byte of headroom instead of 2.
class Stm32CanSafetyBroadcaster {
public:
    // source: where to read Safe1/Safe2 from (e.g. a
    // Stm32L4SafetyRelay&) — must outlive this object, as must
    // transport1/transport2. serialCode: the 32-bit identity value;
    // split once, at construction, into the two 16-bit halves each
    // channel's frame carries.
    Stm32CanSafetyBroadcaster(Safe& source, iTransport& transport1, iTransport& transport2,
                               uint32_t serialCode);

    // Builds and sends both channel frames for the CURRENT state of
    // source_, incrementing the shared sequence counter once
    // afterward. Call this periodically (e.g. from the same loop
    // that calls Stm32L4SafetyRelay::runVerificationRound()) — this
    // does not happen automatically off source_'s own callback,
    // matching the "explicit, polled" convention several other
    // classes in this codebase already use (UartLoopbackChannelSafeInput's
    // own poll(), SafeZone's own EvaluateSafe()).
    void broadcast();

private:
    void buildFrame(uint8_t* outFrame, uint16_t codeHalf, bool safe) const;

    Safe&       source_;
    iTransport& transport1_;
    iTransport& transport2_;
    uint16_t    lowerHalf_;
    uint16_t    upperHalf_;
    uint8_t     sequenceCounter_ = 0;
};
