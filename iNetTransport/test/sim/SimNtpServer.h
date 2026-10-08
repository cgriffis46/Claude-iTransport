// An NTP server for the host tests: answers client requests with its
// own clock (unixMs, which the test sets), holding each request for
// holdMs, and can play an unsynchronised server or send a
// kiss-o'-death. Independent of SntpClient's own code.
#pragma once
#include <atomic>
#include <cstdint>
#include <vector>
#include "NetTypes.h"

class SimNtpServer {
public:
    IpAddress server{192, 168, 1, 123};
    uint64_t unixMs = 1791460800000ull;   // 2026-10-08 12:00:00 UTC
    uint32_t holdMs = 0;                  // between its receive and transmit timestamps
    uint8_t  stratum = 2;
    bool     kod = false;                 // reply with stratum 0, "RATE"
    bool     unsynced = false;            // leap indicator 3
    bool     silent = false;

    std::atomic<int> requests{0};

    std::vector<uint8_t> handle(const uint8_t* p, size_t len) {
        std::vector<uint8_t> out;
        if (len < 48 || (p[0] & 7) != 3) return out;
        ++requests;
        if (silent) return out;
        out.assign(48, 0);
        out[0] = static_cast<uint8_t>(((unsynced ? 3 : 0) << 6) | (4 << 3) | 4);
        out[1] = kod ? 0 : stratum;
        out[2] = 6;
        out[3] = 0xEC;          // precision about 2^-20 s
        if (kod) { out[12] = 'R'; out[13] = 'A'; out[14] = 'T'; out[15] = 'E'; }
        for (int i = 0; i < 8; ++i) out[24 + i] = p[40 + i];    // originate = their transmit
        put(out, 16, unixMs - 30000);                          // reference
        put(out, 32, unixMs);                                  // receive
        put(out, 40, unixMs + holdMs);                         // transmit
        return out;
    }

    static void put(std::vector<uint8_t>& v, size_t at, uint64_t ms) {
        const uint32_t sec = static_cast<uint32_t>(ms / 1000 + 2208988800ull);   // wraps in 2036, as NTP does
        const uint32_t frac = static_cast<uint32_t>(((ms % 1000) << 32) / 1000);
        for (int i = 0; i < 4; ++i) v[at + i] = static_cast<uint8_t>(sec >> (24 - 8 * i));
        for (int i = 0; i < 4; ++i) v[at + 4 + i] = static_cast<uint8_t>(frac >> (24 - 8 * i));
    }
};
