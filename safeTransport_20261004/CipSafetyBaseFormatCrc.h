#pragma once
#include <cstdint>
#include <cstddef>
#include "CrcS3.h"

// Computes the "3 to 250 Byte Data Section, Base Format" CRCs used by
// CIP Safety (ODVA Volume 5, FRS41-43), chaining CRC-S3 over the
// Producer Identifier (PID), a masked Mode Byte, and the data bytes.
//
// PID: confirmed as a 32-bit (UDINT) value, little-endian, established
// per-connection per-direction during Forward_Open/SafetyOpen (ODVA
// Volume 1, Table 3-5.16/3-5.17 — "This is the originator's CIP
// Produced Connection ID" / "...Consumed Connection ID"). Never
// transmitted in the safety data frame itself — fed into the CRC
// purely as an implicit, pre-shared seed, so a message from the wrong
// sender produces a CRC mismatch rather than needing an explicit
// sender-ID field on the wire (Volume 5, Section 2-1.2.3, Message
// Insertion).
//
// OPEN ASSUMPTION, clearly flagged rather than silently assumed
// correct: see kAssumedInitialSeed below.
class CipSafetyBaseFormatCrc {
public:
    // The CRC-S3 register's starting value BEFORE PID is fed in.
    // NEITHER Volume 5's prose (the entirety of Chapter 2's CRC
    // section, 2-1.7.1.1 through 2-1.7.1.8) NOR its example code
    // (Appendix E, checked through its actual end at page E-17)
    // states this explicitly as an isolated number for the RUNTIME
    // protocol case. Appendix E's own code does show 0xFFFF being
    // used — but only to validate the CRC table implementation
    // against the spec's published "Check" test vector, a separate,
    // self-test-only convention (Table E-1.1's own footnote 2), not
    // the runtime seed. 0x0000 is the standard default for a CRC
    // whose Init is catalogued as "Variable" with no protocol-
    // specific value stated elsewhere in the two ODVA documents
    // checked (Volume 5: CIP Safety, Volume 1: Common Industrial
    // Protocol). If real hardware interop testing ever shows a CRC
    // mismatch against a genuine CIP Safety device with everything
    // else here correct, THIS is the first value to reconsider.
    static constexpr uint16_t kAssumedInitialSeed = 0x0000u;

    // Computes the Actual Data CRC (FRS42): PID (4 bytes, little-
    // endian), then (modeByte & 0xE0), then the actual data bytes —
    // all fed through CRC-S3 in that order, each stage's output
    // becoming the next stage's seed (the exact chaining pattern
    // Appendix E's own ComputeCRCS3RefN functions are built around).
    static uint16_t computeActualDataCrc(uint32_t pid, uint8_t modeByte,
                                          const uint8_t* data, size_t dataLen) {
        uint8_t pidBytes[4];
        encodePidLittleEndian(pid, pidBytes);

        uint16_t crc = CrcS3::compute(pidBytes, 4, kAssumedInitialSeed);

        const uint8_t maskedModeByte = static_cast<uint8_t>(modeByte & 0xE0u);
        crc = CrcS3::compute(&maskedModeByte, 1, crc);

        crc = CrcS3::compute(data, dataLen, crc);
        return crc;
    }

    // Computes the Complement Data CRC (FRS43): PID, then
    // (modeByte XOR 0xFF) & 0xE0, then the COMPLEMENTED (bitwise-
    // inverted) data bytes — same chaining, different inputs.
    // complementedData must already hold data's bitwise complement
    // (each byte XORed with 0xFF); this function doesn't invert it
    // for you, since the Base Format frame actually transmits the
    // complemented bytes as a separate field (Figure 2-1.11) —
    // unlike the smaller 1-2 byte format, where the complement is
    // only implied via its own CRC, never transmitted directly.
    static uint16_t computeComplementDataCrc(uint32_t pid, uint8_t modeByte,
                                              const uint8_t* complementedData, size_t dataLen) {
        uint8_t pidBytes[4];
        encodePidLittleEndian(pid, pidBytes);

        uint16_t crc = CrcS3::compute(pidBytes, 4, kAssumedInitialSeed);

        const uint8_t maskedInvertedModeByte = static_cast<uint8_t>((modeByte ^ 0xFFu) & 0xE0u);
        crc = CrcS3::compute(&maskedInvertedModeByte, 1, crc);

        crc = CrcS3::compute(complementedData, dataLen, crc);
        return crc;
    }

private:
    static void encodePidLittleEndian(uint32_t pid, uint8_t out[4]) {
        out[0] = static_cast<uint8_t>(pid & 0xFFu);
        out[1] = static_cast<uint8_t>((pid >> 8) & 0xFFu);
        out[2] = static_cast<uint8_t>((pid >> 16) & 0xFFu);
        out[3] = static_cast<uint8_t>((pid >> 24) & 0xFFu);
    }
};
