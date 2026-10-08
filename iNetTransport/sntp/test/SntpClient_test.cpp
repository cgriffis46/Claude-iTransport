// Host test for SntpClient against the simulated server in
// test/sim/SimNtpServer.h, on a simulated clock.
//
//   g++ -std=c++14 -Wall -Wextra -I../inc -I../../inc -I../../test/sim
//       SntpClient_test.cpp ../src/SntpClient.cpp -o SntpClient_test
#include <cstdio>
#include <vector>
#include "SntpClient.h"
#include "SimNtpServer.h"

static int g_failures = 0;
static void check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failures;
}

struct Rig {
    SntpClient sntp;
    SimNtpServer srv;
    uint32_t now = 50000;
    uint32_t oneWay = 15;           // network delay each way, ms
    uint8_t buf[SntpClient::kPacket];
    std::vector<uint8_t> lastReq;
    int sent = 0, synced = 0, failed = 0;
    bool loseReplies = false;

    void count(const SntpClient::Action& a) {
        if (a.event == SntpClient::Event::Synced) ++synced;
        if (a.event == SntpClient::Event::Failed) ++failed;
    }
    // One exchange: the request goes out at now, reaches the server
    // oneWay later, is held, and the reply takes oneWay to come back.
    void run(uint32_t ms) {
        for (uint32_t t = 0; t < ms; t += 10, now += 10) {
            SntpClient::Action a = sntp.poll(now, buf, sizeof buf);
            count(a);
            if (a.len == 0) continue;
            ++sent;
            lastReq.assign(buf, buf + a.len);
            srv.unixMs += oneWay;
            const std::vector<uint8_t> reply = srv.handle(buf, a.len);
            srv.unixMs -= oneWay;
            if (reply.empty() || loseReplies) continue;
            count(sntp.receive(reply.data(), reply.size(), now + 2 * oneWay + srv.holdMs));
        }
    }
};

int main() {
    std::printf("request and answer\n");
    {
        Rig r;
        r.srv.holdMs = 4;
        check(r.sntp.begin(r.srv.server, 0xC0FFEE, r.now), "begin()");
        r.run(10);
        check(r.lastReq.size() == 48 && r.lastReq[0] == 0x23, "48 bytes, version 4, client mode");
        bool nonce = false;
        for (int i = 40; i < 48; ++i) nonce |= r.lastReq[i] != 0;
        check(nonce, "a random transmit timestamp, for the server to echo");
        check(r.synced == 1 && !r.sntp.busy(), "Synced");
        // The server read its clock 15 ms after we sent (its unixMs is
        // ours at the moment of sending), held it 4 ms, and the answer
        // took 15 ms back: at atMs() the time was unixMs + 15 + 4 + 15.
        check(r.sntp.atMs() == 50000 + 34, "atMs(): when the answer arrived");
        check(r.sntp.rttMs() == 30, "round trip, less the server's 4 ms");
        check(r.sntp.unixMs() == r.srv.unixMs + 34, "unixMs(): exact to the ms");
        check(r.sntp.stratum() == 2, "stratum");
    }

    std::printf("NTP timestamps\n");
    {
        check(SntpClient::ntpToUnixMs(0xE95A0000u, 0) == (0xE95A0000ull - 2208988800ull) * 1000, "era 0");
        check(SntpClient::ntpToUnixMs(0x83AA7E80u, 0) == 0, "1970-01-01");
        check(SntpClient::ntpToUnixMs(0, 0) == 2085978496000ull, "0 is 2036-02-07 06:28:16, not 1900");
        check(SntpClient::ntpToUnixMs(0x83AA7E80u, 0x80000000u) == 500, "fraction to ms");
        Rig r;
        r.srv.unixMs = 2085978496000ull + 3600000;   // an hour after the 2036 rollover
        r.oneWay = 0;
        r.sntp.begin(r.srv.server, 1, r.now);
        r.run(10);
        check(r.synced == 1 && r.sntp.unixMs() == r.srv.unixMs, "a server past 2036 is read right");
    }

    std::printf("refusals\n");
    {
        Rig r;
        r.srv.kod = true;
        r.sntp.begin(r.srv.server, 2, r.now);
        r.run(5000);
        check(r.failed == 1 && r.sntp.kissOfDeath() && r.sent == 1, "kiss-o'-death: Failed at once, not retried");
        r.srv.kod = false;
        r.srv.unsynced = true;
        r.sntp.begin(r.srv.server, 3, r.now);
        r.run(10);
        check(r.failed == 2 && r.synced == 0, "unsynchronised server (LI 3): Failed");
        check(!r.sntp.begin(IpAddress(), 4, r.now), "no server: refused");
    }

    std::printf("ignores what isn't its answer\n");
    {
        Rig r;
        r.loseReplies = true;
        r.sntp.begin(r.srv.server, 5, r.now);
        r.run(10);
        std::vector<uint8_t> reply = r.srv.handle(r.lastReq.data(), r.lastReq.size());
        std::vector<uint8_t> bad = reply;
        bad[30] ^= 1;
        r.count(r.sntp.receive(bad.data(), bad.size(), r.now));
        check(r.sntp.busy() && r.synced == 0, "originate isn't our transmit timestamp");
        bad = reply;
        bad[0] = (bad[0] & ~7) | 3;
        r.count(r.sntp.receive(bad.data(), bad.size(), r.now));
        check(r.sntp.busy() && r.synced == 0, "client mode, not server");
        r.count(r.sntp.receive(reply.data(), 47, r.now));
        check(r.sntp.busy() && r.synced == 0, "short");
        r.run(2000);   // the retry has a new nonce...
        check(r.sent == 2, "retried after 2 s");
        r.count(r.sntp.receive(reply.data(), reply.size(), r.now));
        check(r.sntp.busy() && r.synced == 0, "...so a late answer to the first try doesn't count");
        r.run(2000);
        r.run(4000);
        check(r.failed == 1 && r.sent == 3, "three tries, then Failed");
    }

    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "PASSED", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
