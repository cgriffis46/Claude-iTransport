#pragma once
#include <cstdint>
#include <cstddef>

// Standard CRC-8, polynomial 0x07, init 0x00, no reflection — shared
// utility so the algorithm isn't duplicated between
// Stm32L4SafetyRelay and Stm32CanSafetyBroadcaster (and anything else
// that needs it later). A simple, bit-by-bit implementation,
// deliberately not a lookup table: for the small messages this
// codebase uses it for, the speed difference is irrelevant, and a
// straightforward implementation is easier to visually verify than a
// generated table, which matters more here than raw speed.
//
// Used entirely internally: the SAME function both generates a CRC
// when sending and validates it when receiving, so it doesn't need
// to match any external/named CRC-8 standard's exact test vectors to
// be correct — it only needs to reliably catch corruption between
// transmission and reception, verified directly against its own
// send/receive pair.
class Crc8 {
public:
    static uint8_t compute(const uint8_t* data, size_t len) {
        uint8_t crc = 0x00;
        for (size_t i = 0; i < len; ++i) {
            crc ^= data[i];
            for (int bit = 0; bit < 8; ++bit) {
                if (crc & 0x80) {
                    crc = static_cast<uint8_t>((crc << 1) ^ 0x07);
                } else {
                    crc = static_cast<uint8_t>(crc << 1);
                }
            }
        }
        return crc;
    }
};
