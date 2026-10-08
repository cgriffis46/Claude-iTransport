// Host test for DhcpClient against the simulated server in
// test/sim/SimDhcpServer.h, on a simulated clock.
//
//   g++ -std=c++17 -Wall -Wextra -I../inc -I../../inc -I../../test/sim
//       DhcpClient_test.cpp ../src/DhcpClient.cpp -o DhcpClient_test
#include <cstdio>
#include <vector>
#include "DhcpClient.h"
#include "SimDhcpServer.h"

static int g_failures = 0;
static void check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failures;
}

static const MacAddress kMac(0x02, 0x08, 0xDC, 0xAA, 0xBB, 0xCC);
static const IpAddress kBroadcast(255, 255, 255, 255);

struct Rig {
    DhcpClient dhcp;
    SimDhcpServer srv;
    uint32_t now = 5000;
    uint8_t buf[600];
    int sent = 0, bound = 0, lost = 0;
    IpAddress lastDst;
    std::vector<uint8_t> dropNext; // reply withheld by a "lost packet"
    bool loseReplies = false;

    // Runs ms of simulated time, 10 ms a step, passing packets both ways.
    void run(uint32_t ms) {
        for (uint32_t t = 0; t < ms; t += 10, now += 10) {
            DhcpClient::Action a = dhcp.poll(now, buf, sizeof buf);
            count(a);
            if (a.len == 0) continue;
            ++sent;
            lastDst = a.dst;
            const std::vector<uint8_t> reply = srv.handle(buf, a.len);
            if (reply.empty() || loseReplies) continue;
            count(dhcp.receive(reply.data(), reply.size(), now));
        }
    }
    void count(const DhcpClient::Action& a) {
        if (a.event == DhcpClient::Event::Bound) ++bound;
        if (a.event == DhcpClient::Event::Lost) ++lost;
    }
};

int main() {
    std::printf("DISCOVER, OFFER, REQUEST, ACK\n");
    {
        Rig r;
        r.dhcp.begin(kMac, 1234, r.now);
        r.run(20);
        check(r.srv.discovers == 1 && r.srv.requests == 1, "one DISCOVER, one REQUEST");
        check(r.srv.lastBroadcast && r.lastDst == kBroadcast, "broadcast, broadcast-reply flag set");
        check(r.srv.lastHadServerId && r.srv.lastServerId == r.srv.server, "REQUEST names the offering server");
        check(r.srv.lastRequestedIp == r.srv.offerIp, "and asks for the offered address");
        check(r.srv.lastLen == DhcpClient::kTxPacket, "300-byte packets (BOOTP minimum)");
        check(r.bound == 1 && r.dhcp.state() == DhcpClient::State::Bound, "Bound");
        const NetConfig& l = r.dhcp.lease();
        check(l.ip == r.srv.offerIp && l.subnet == r.srv.subnet && l.gateway == r.srv.router && l.dns == r.srv.dns,
              "lease: address, mask, first router, DNS");
        check(l.ntp == r.srv.ntp, "and the NTP server (option 42)");
        check(r.dhcp.leaseSeconds() == 3600, "lease time");
        check(r.dhcp.nextWakeMs(r.now) > 1790000 && r.dhcp.nextWakeMs(r.now) <= 1800000, "next wake at T1, half the lease");
    }

    std::printf("retransmission\n");
    {
        Rig r;
        r.srv.silent = true;
        r.dhcp.begin(kMac, 1, r.now);
        r.run(10);
        check(r.sent == 1, "first DISCOVER at once");
        r.run(2000);
        check(r.sent == 2, "again after 2 s");
        r.run(4000);
        check(r.sent == 3, "then 4 s");
        r.run(8000);
        check(r.sent == 4, "then 8 s");
        r.run(200000);
        check(r.sent < 12, "backing off to at most every 32 s");
        r.srv.silent = false;
        r.run(33000);
        check(r.bound == 1, "binds once a server answers");
    }

    std::printf("renew and rebind\n");
    {
        Rig r;
        r.srv.leaseSec = 100;
        r.dhcp.begin(kMac, 7, r.now);
        r.run(20);
        const int requests = r.srv.requests;
        r.run(49000);
        check(r.srv.requests == requests, "nothing before T1 (50 s)");
        r.run(2000);
        check(r.srv.requests == requests + 1, "REQUEST at T1");
        check(!r.srv.lastBroadcast && r.lastDst == r.srv.server && r.srv.lastCiaddr == r.srv.offerIp,
              "renewing: unicast to the server, ciaddr set");
        check(!r.srv.lastHadServerId, "no server id when renewing");
        check(r.bound == 2 && r.dhcp.state() == DhcpClient::State::Bound, "renewed: Bound again");

        r.loseReplies = true; // server gone
        r.run(50000 + 37500 + 100);
        check(r.dhcp.state() == DhcpClient::State::Rebinding && r.lastDst == kBroadcast && r.srv.lastCiaddr == r.srv.offerIp,
              "past T2: rebinding, broadcast");
        check(r.lost == 0, "still bound");
        r.run(13000);
        check(r.lost == 1 && r.dhcp.state() == DhcpClient::State::Selecting, "lease expired: Lost, back to DISCOVER");
        check(r.srv.lastType == 1 && r.srv.lastRequestedIp == r.srv.offerIp, "DISCOVER asks for the old address back");
        r.loseReplies = false;
        r.run(40000);
        check(r.bound == 3, "and binds again");
    }

    std::printf("NAK\n");
    {
        Rig r;
        r.srv.nak = true;
        r.dhcp.begin(kMac, 9, r.now);
        r.run(20);
        check(r.bound == 0 && r.dhcp.state() == DhcpClient::State::Selecting, "NAK to REQUEST: start over");
        r.srv.nak = false;
        r.run(3000);
        check(r.bound == 1, "then binds");
        r.srv.nak = true;
        r.dhcp.linkRestored(r.now);
        r.run(20);
        check(r.lost == 1, "link back, lease NAKed here: Lost");
    }

    std::printf("link restored on the same network\n");
    {
        Rig r;
        r.dhcp.begin(kMac, 11, r.now);
        r.run(20);
        const int requests = r.srv.requests;
        r.dhcp.linkRestored(r.now);
        r.run(20);
        check(r.srv.requests == requests + 1 && r.srv.lastBroadcast && r.srv.lastCiaddr == r.srv.offerIp,
              "REQUEST broadcast at once");
        check(r.lost == 0 && r.bound == 2, "confirmed, still bound");
    }

    std::printf("ignores what isn't for it\n");
    {
        Rig r;
        r.srv.silent = true;
        r.dhcp.begin(kMac, 3, r.now);
        r.run(10);
        r.srv.silent = false;
        std::vector<uint8_t> offer = r.srv.handle(r.buf, DhcpClient::kTxPacket);
        std::vector<uint8_t> bad = offer;
        bad[4] ^= 1;
        r.dhcp.receive(bad.data(), bad.size(), r.now);
        check(r.dhcp.state() == DhcpClient::State::Selecting, "wrong transaction id");
        bad = offer;
        bad[33] ^= 1;
        r.dhcp.receive(bad.data(), bad.size(), r.now);
        check(r.dhcp.state() == DhcpClient::State::Selecting, "someone else's MAC");
        bad = offer;
        bad[241] = 200; // option length past the end
        r.dhcp.receive(bad.data(), bad.size(), r.now);
        check(r.dhcp.state() == DhcpClient::State::Selecting, "malformed options");
        r.dhcp.receive(offer.data(), 100, r.now);
        check(r.dhcp.state() == DhcpClient::State::Selecting, "truncated");
        r.dhcp.receive(offer.data(), offer.size(), r.now);
        check(r.dhcp.state() == DhcpClient::State::Requesting, "the real OFFER is taken");
    }

    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "PASSED", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
