// A DNS server for the host tests: answers A queries from a table,
// follows CNAMEs (answering with a compressed name, as real servers
// do), says NXDOMAIN for unknown names, and records what it was asked.
// Independent of DnsClient's own code, so the two check each other.
#pragma once
#include <atomic>
#include <cctype>
#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include "NetTypes.h"

class SimDnsServer {
public:
    IpAddress server{192, 168, 1, 53};
    std::map<std::string, IpAddress> a;           // lower-case name -> address
    std::map<std::string, std::string> cname;     // lower-case name -> target
    bool    silent = false;     // answer nothing
    uint8_t rcode = 0;          // force this RCODE (2 SERVFAIL, ...)

    std::atomic<int> queries{0};
    std::string lastName;
    uint16_t lastId = 0;

    SimDnsServer() {
        a["pool.ntp.org"] = IpAddress(162, 159, 200, 1);
        cname["www.example.com"] = "example.edgekey.net";
        a["example.edgekey.net"] = IpAddress(93, 184, 216, 34);
    }

    std::vector<uint8_t> handle(const uint8_t* p, size_t len) {
        std::vector<uint8_t> out;
        if (len < 17) return out;
        lastId = static_cast<uint16_t>((p[0] << 8) | p[1]);
        size_t at = 12;
        std::string name;
        while (at < len && p[at] != 0) {
            const uint8_t n = p[at++];
            if (!name.empty()) name += '.';
            for (uint8_t i = 0; i < n && at < len; ++i) name += static_cast<char>(std::tolower(p[at++]));
        }
        ++at;                       // the root label
        const size_t qEnd = at + 4; // type and class
        if (qEnd > len) return out;
        ++queries;
        lastName = name;
        if (silent) return out;

        out.assign(p, p + qEnd);    // header and question, as asked
        out[2] = 0x81;              // QR, RD
        out[3] = 0x80;              // RA
        out[6] = out[7] = 0;        // answers, set below
        out[8] = out[9] = out[10] = out[11] = 0;
        if (rcode) { out[3] |= rcode; return out; }

        uint16_t answers = 0;
        size_t namePtr = 12;        // where the name being answered is, for compression
        std::string at_name = name;
        for (int hops = 0; hops < 4 && cname.count(at_name); ++hops) {
            const std::string target = cname[at_name];
            put16(out, 0xC000 | static_cast<uint16_t>(namePtr));
            put16(out, 5); put16(out, 1); put32(out, 300);
            const std::vector<uint8_t> enc = encode(target);
            put16(out, static_cast<uint16_t>(enc.size()));
            namePtr = out.size();
            out.insert(out.end(), enc.begin(), enc.end());
            at_name = target;
            ++answers;
        }
        auto it = a.find(at_name);
        if (it != a.end()) {
            put16(out, 0xC000 | static_cast<uint16_t>(namePtr));
            put16(out, 1); put16(out, 1); put32(out, 300); put16(out, 4);
            out.insert(out.end(), it->second.b, it->second.b + 4);
            ++answers;
        } else if (answers == 0) {
            out[3] |= 3;            // NXDOMAIN
        }
        out[6] = static_cast<uint8_t>(answers >> 8);
        out[7] = static_cast<uint8_t>(answers);
        return out;
    }

private:
    static void put16(std::vector<uint8_t>& v, uint16_t x) { v.push_back(uint8_t(x >> 8)); v.push_back(uint8_t(x)); }
    static void put32(std::vector<uint8_t>& v, uint32_t x) { put16(v, uint16_t(x >> 16)); put16(v, uint16_t(x)); }
    static std::vector<uint8_t> encode(const std::string& n) {
        std::vector<uint8_t> v;
        size_t s = 0;
        while (s <= n.size()) {
            size_t d = n.find('.', s);
            if (d == std::string::npos) d = n.size();
            v.push_back(static_cast<uint8_t>(d - s));
            v.insert(v.end(), n.begin() + static_cast<long>(s), n.begin() + static_cast<long>(d));
            s = d + 1;
        }
        v.push_back(0);
        return v;
    }
};
