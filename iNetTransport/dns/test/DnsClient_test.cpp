// Host test for DnsClient against the simulated server in
// test/sim/SimDnsServer.h, on a simulated clock.
//
//   g++ -std=c++14 -Wall -Wextra -I../inc -I../../inc -I../../test/sim
//       DnsClient_test.cpp ../src/DnsClient.cpp -o DnsClient_test
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "DnsClient.h"
#include "SimDnsServer.h"

static int g_failures = 0;
static void check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failures;
}

struct Rig {
    DnsClient dns;
    SimDnsServer srv;
    uint32_t now = 1000;
    uint8_t buf[DnsClient::kMaxQuery];
    std::vector<uint8_t> lastQuery;
    int sent = 0, resolved = 0, failed = 0;
    bool loseReplies = false;

    void count(const DnsClient::Action& a) {
        if (a.event == DnsClient::Event::Resolved) ++resolved;
        if (a.event == DnsClient::Event::Failed) ++failed;
    }
    void run(uint32_t ms) {
        for (uint32_t t = 0; t < ms; t += 10, now += 10) {
            DnsClient::Action a = dns.poll(now, buf, sizeof buf);
            count(a);
            if (a.len == 0) continue;
            ++sent;
            lastQuery.assign(buf, buf + a.len);
            const std::vector<uint8_t> reply = srv.handle(buf, a.len);
            if (!reply.empty() && !loseReplies) count(dns.receive(reply.data(), reply.size(), now));
        }
    }
};

int main() {
    std::printf("query\n");
    {
        Rig r;
        check(r.dns.begin("pool.ntp.org", r.srv.server, 0x1234, r.now), "begin()");
        r.run(10);
        const uint8_t want[] = {0x12, 0x34, 0x01, 0x00, 0, 1, 0, 0, 0, 0, 0, 0,
                                4, 'p', 'o', 'o', 'l', 3, 'n', 't', 'p', 3, 'o', 'r', 'g', 0, 0, 1, 0, 1};
        check(r.lastQuery.size() == sizeof want && std::memcmp(r.lastQuery.data(), want, sizeof want) == 0,
              "ID, recursion desired, one question, labels, type A, class IN");
        check(r.resolved == 1 && r.dns.address() == IpAddress(162, 159, 200, 1) && !r.dns.busy(), "Resolved");
    }

    std::printf("answers\n");
    {
        Rig r;
        r.dns.begin("WWW.Example.COM.", r.srv.server, 7, r.now);
        r.run(10);
        check(r.srv.lastName == "www.example.com", "trailing dot dropped; asked as given");
        check(r.resolved == 1 && r.dns.address() == IpAddress(93, 184, 216, 34),
              "CNAME followed to the A record, through compressed names; question matched case-insensitively");

        r.dns.begin("nosuch.example", r.srv.server, 8, r.now);
        r.run(10);
        check(r.failed == 1 && r.dns.error() == DnsClient::Error::NotFound, "NXDOMAIN: Failed, NotFound");

        r.srv.rcode = 2;
        r.dns.begin("pool.ntp.org", r.srv.server, 9, r.now);
        r.run(10);
        check(r.failed == 2 && r.dns.error() == DnsClient::Error::ServerFailure, "SERVFAIL: Failed, ServerFailure");
        r.srv.rcode = 0;

        r.srv.cname["dangling.example"] = "nowhere.example";
        r.dns.begin("dangling.example", r.srv.server, 10, r.now);
        r.run(10);
        check(r.failed == 3 && r.dns.error() == DnsClient::Error::NotFound, "a CNAME with no address behind it: NotFound");
    }

    std::printf("ignores what isn't its answer\n");
    {
        Rig r;
        r.srv.silent = true;
        r.dns.begin("pool.ntp.org", r.srv.server, 0x55AA, r.now);
        r.run(10);
        r.srv.silent = false;
        std::vector<uint8_t> reply = r.srv.handle(r.lastQuery.data(), r.lastQuery.size());
        std::vector<uint8_t> bad = reply;
        bad[1] ^= 1;
        r.count(r.dns.receive(bad.data(), bad.size(), r.now));
        check(r.dns.busy() && r.resolved == 0, "another ID");
        bad = reply;
        bad[13] = 'x';
        r.count(r.dns.receive(bad.data(), bad.size(), r.now));
        check(r.dns.busy() && r.resolved == 0, "another question");
        bad = reply;
        bad[2] &= 0x7F;
        r.count(r.dns.receive(bad.data(), bad.size(), r.now));
        check(r.dns.busy() && r.resolved == 0, "a query, not a response");
        r.count(r.dns.receive(reply.data(), 20, r.now));
        check(r.dns.busy() && r.resolved == 0, "truncated in the question");
        bad = reply;
        bad.resize(bad.size() - 3);   // the address cut short
        r.count(r.dns.receive(bad.data(), bad.size(), r.now));
        check(!r.dns.busy() && r.failed == 1, "truncated answer: Failed, no crash");
        r.dns.begin("pool.ntp.org", r.srv.server, 0x55AA, r.now);
        r.count(r.dns.receive(reply.data(), reply.size(), r.now));
        check(r.resolved == 1, "the real answer is taken");
    }

    std::printf("retries\n");
    {
        Rig r;
        r.loseReplies = true;
        r.dns.begin("pool.ntp.org", r.srv.server, 1, r.now);
        r.run(10);
        check(r.sent == 1, "first query at once");
        r.run(2000);
        check(r.sent == 2, "again after 2 s");
        r.run(3000);
        check(r.sent == 3 && r.failed == 0, "and after 3 s more");
        r.run(4900);
        check(r.failed == 0, "still waiting at 9.9 s");
        r.run(200);
        check(r.failed == 1 && r.dns.error() == DnsClient::Error::Timeout && r.sent == 3, "Failed, Timeout, after 10 s");
        check(r.dns.nextWakeMs(r.now) == 0xFFFFFFFFu, "then nothing to do");
    }

    std::printf("refused at the start\n");
    {
        DnsClient dns;
        const IpAddress srv(1, 1, 1, 1);
        check(!dns.begin("", srv, 1, 0) && dns.error() == DnsClient::Error::BadName, "empty name");
        check(!dns.begin("a..b", srv, 1, 0) && dns.error() == DnsClient::Error::BadName, "empty label");
        check(!dns.begin(std::string(64, 'a').c_str(), srv, 1, 0), "64-character label");
        check(dns.begin(std::string(63, 'a').c_str(), srv, 1, 0), "63-character label is fine");
        std::string longName;
        while (longName.size() < 254) longName += "abcdefghi.";
        longName.resize(254);
        check(!dns.begin(longName.c_str(), srv, 1, 0), "over 253 characters");
        check(!dns.begin("pool.ntp.org", IpAddress(), 1, 0) && dns.error() == DnsClient::Error::NoServer, "no server");
        check(!dns.busy(), "and nothing started");
    }

    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "PASSED", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
