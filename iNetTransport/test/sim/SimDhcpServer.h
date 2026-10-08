// A DHCP server for the host tests: answers DISCOVER with an OFFER and
// REQUEST with an ACK (or a NAK, when told to), and records what it
// was sent. Independent of DhcpClient's own code, so the two check
// each other.
#pragma once
#include <cstdint>
#include <cstring>
#include <vector>
#include "NetTypes.h"

class SimDhcpServer {
public:
    IpAddress server{192, 168, 1, 1};
    IpAddress offerIp{192, 168, 1, 77};
    IpAddress subnet{255, 255, 255, 0};
    IpAddress router{192, 168, 1, 254};
    IpAddress dns{8, 8, 8, 8};
    IpAddress ntp{192, 168, 1, 123};  // option 42; all zeros: not offered
    uint32_t  leaseSec = 3600;
    bool      nak = false;      // NAK every REQUEST
    bool      silent = false;   // answer nothing

    // What the last request carried.
    int       discovers = 0, requests = 0;
    uint8_t   lastType = 0;
    bool      lastBroadcast = false;
    IpAddress lastCiaddr, lastRequestedIp, lastServerId;
    bool      lastHadServerId = false;
    size_t    lastLen = 0;

    // The reply to a request (empty for none).
    std::vector<uint8_t> handle(const uint8_t* p, size_t len) {
        std::vector<uint8_t> out;
        if (len < 240 || p[0] != 1) return out;
        lastLen = len;
        lastType = 0;
        lastHadServerId = false;
        lastRequestedIp = IpAddress();
        size_t i = 240;
        while (i + 1 < len && p[i] != 255) {
            if (p[i] == 0) { ++i; continue; }
            const uint8_t code = p[i], n = p[i + 1];
            const uint8_t* v = p + i + 2;
            if (code == 53) lastType = v[0];
            if (code == 50) lastRequestedIp = IpAddress(v[0], v[1], v[2], v[3]);
            if (code == 54) { lastServerId = IpAddress(v[0], v[1], v[2], v[3]); lastHadServerId = true; }
            i += 2 + n;
        }
        lastBroadcast = (p[10] & 0x80) != 0;
        lastCiaddr = IpAddress(p[12], p[13], p[14], p[15]);
        uint8_t type;
        if (lastType == 1) { ++discovers; type = 2; }
        else if (lastType == 3) { ++requests; type = nak ? 6 : 5; }
        else return out;
        if (silent) return out;

        out.assign(300, 0);
        out[0] = 2; out[1] = 1; out[2] = 6;
        std::memcpy(&out[4], p + 4, 4);    // xid
        out[10] = p[10];                   // flags
        if (type != 6) std::memcpy(&out[16], offerIp.b, 4);
        std::memcpy(&out[20], server.b, 4);
        std::memcpy(&out[28], p + 28, 16); // chaddr
        const uint8_t magic[4] = {99, 130, 83, 99};
        std::memcpy(&out[236], magic, 4);
        size_t o = 240;
        auto opt = [&](uint8_t code, const uint8_t* v, uint8_t n) {
            out[o++] = code; out[o++] = n;
            std::memcpy(&out[o], v, n); o += n;
        };
        opt(53, &type, 1);
        opt(54, server.b, 4);
        if (type != 6) {
            const uint8_t l[4] = {uint8_t(leaseSec >> 24), uint8_t(leaseSec >> 16), uint8_t(leaseSec >> 8), uint8_t(leaseSec)};
            opt(51, l, 4);
            opt(1, subnet.b, 4);
            const uint8_t routers[8] = {router.b[0], router.b[1], router.b[2], router.b[3], 10, 0, 0, 1};
            opt(3, routers, 8); // two routers: the client takes the first
            opt(6, dns.b, 4);
            if (!ntp.isZero()) opt(42, ntp.b, 4);
        }
        out[o++] = 255;
        return out;
    }
};
