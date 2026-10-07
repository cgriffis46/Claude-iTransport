#pragma once
#include <cstdint>
#include <cstddef>

// A minimal length-prefix framing scheme for splitting a raw TCP byte
// stream into discrete CIP Message Router Request/Response buffers.
//
// IMPORTANT, stated plainly rather than left implicit: this is NOT
// the real ODVA EtherNet/IP encapsulation protocol. The genuine
// encapsulation header (with its own length field, command codes, and
// session handles) is defined in ODVA's Volume 2 (EtherNet/IP
// Adaptation of CIP), which this project does not have — and
// inventing bytes to match a spec we can't actually check against
// would be exactly the kind of unverified guess this project has
// tried hard to avoid elsewhere. What's actually needed here is much
// narrower: SOME way to know where one CipTagMessageCodec request
// ends and the next begins, since raw TCP provides no message
// boundaries at all. This provides exactly that, and nothing more —
// a real EtherNet/IP-compliant encapsulation layer would need to
// replace this before this server could interoperate with a
// commercial CIP scanner or engineering tool.
//
// Format: a 2-byte little-endian payload length, followed by exactly
// that many bytes of payload (a complete CipTagMessageCodec request
// or response). kMaxPayloadLen bounds this: large enough for any
// realistic tag read/write, small enough to keep per-connection
// buffers modest on a resource-constrained MCU.
namespace CipFrame {
    constexpr size_t kMaxPayloadLen = 256;
    constexpr size_t kHeaderLen = 2;

    inline void encodeHeader(uint16_t payloadLen, uint8_t out[kHeaderLen]) {
        out[0] = static_cast<uint8_t>(payloadLen & 0xFFu);        // little-endian, matching
        out[1] = static_cast<uint8_t>((payloadLen >> 8) & 0xFFu); // CIP's own native byte order
    }

    inline uint16_t decodeHeader(const uint8_t in[kHeaderLen]) {
        return static_cast<uint16_t>(in[0]) | (static_cast<uint16_t>(in[1]) << 8);
    }
}
