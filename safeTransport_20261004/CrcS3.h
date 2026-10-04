#pragma once
#include <cstdint>
#include <cstddef>

// CIP Safety's CRC-S3 — a 16-bit CRC, per ODVA's "The CIP Networks
// Library, Volume 5: CIP Safety," Appendix E, Table E-1.1:
//   Width=16, Poly=0x080F, RefIn=False, RefOut=False, XorOut=0x0000
// No input or output reflection: processed MSB-first, matching the
// spec's own "Crc16ComputeSlow" reference model (Appendix E, which
// the spec itself states "the table driven routine must match") —
// this implementation follows that reference model exactly: XOR the
// next byte into the high byte of the register, then shift left 8
// times, conditionally XORing in the polynomial on each 1-bit
// shifted out of the top.
//
// VERIFIED, not assumed: the spec publishes a test vector specifically
// so an implementation can be checked without trusting it blindly —
// Table E-1.1's "Check" value states that CRC-S3 of the ASCII string
// "123456789", computed with an initial register value of 0xFFFF,
// must equal 0x9516. This implementation is confirmed against that
// exact value directly (see the accompanying test), not just built
// to look plausible.
//
// What this class does NOT yet cover: the actual runtime seed
// convention used when assembling a real CIP Safety Base Format Data
// CRC (which involves feeding the Producer Identifier through the
// algorithm first, per Chapter 2) is a separate piece of the puzzle,
// still being confirmed against the spec — this class implements and
// verifies the CRC ALGORITHM ITSELF, standalone. Full frame assembly
// using it correctly is a following step, not implemented here.
class CrcS3 {
public:
    static constexpr uint16_t kPolynomial = 0x080Fu;

    // Computes CRC-S3 over len bytes of data, continuing from the
    // given seed/register value — this shape (taking a seed, not
    // just always starting fresh) exists specifically so multiple
    // calls can be chained to cover data that arrives in separate
    // pieces (e.g. PID bytes, then a mode byte, then payload bytes),
    // each call's output becoming the next call's seed, matching how
    // the spec's own reference code is structured.
    static uint16_t compute(const uint8_t* data, size_t len, uint16_t seed) {
        uint16_t crc = seed;
        for (size_t i = 0; i < len; ++i) {
            crc = static_cast<uint16_t>(crc ^ (static_cast<uint16_t>(data[i]) << 8));
            for (int bit = 0; bit < 8; ++bit) {
                if (crc & 0x8000u) {
                    crc = static_cast<uint16_t>((crc << 1) ^ kPolynomial);
                } else {
                    crc = static_cast<uint16_t>(crc << 1);
                }
            }
        }
        return crc;
    }
};
