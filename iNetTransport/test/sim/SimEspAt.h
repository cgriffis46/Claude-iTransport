// A simulated ESP-AT module (v2.x behaviour) behind a simulated UART,
// for the host tests. It parses the AT commands the driver writes and
// answers the way the firmware does, including the parts that trip
// parsers up: command echo until ATE0, boot noise and "ready" after a
// reset, "busy p...", the ">" prompt with no newline, and binary
// +CIPRECVDATA payloads that may themselves contain "\r\nOK\r\n".
//
// Answers are pushed to the driver's sink synchronously, from inside
// its write() — sooner than any real UART, which is the hard case.
// The test plays the network with the peer*() calls. Thread-safe.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>
#include "iTransport.h"
#include "NetTypes.h"

class SimEspAt {
public:
    // ---- knobs ----
    std::string ssid = "home", pass = "secret";
    IpAddress ip{192, 168, 4, 20}, gateway{192, 168, 4, 1}, netmask{255, 255, 255, 0}, dns{192, 168, 4, 1};
    bool silent = false;        // a dead module: answers nothing
    bool v1RecvFormat = false;  // ESP8266 AT 1.7's "+CIPRECVDATA,<len>:<data>"
    int  busyNext = 0;          // answer the next N commands "busy p..."
    bool refuseConnect = false;

    // ---- what happened ----
    std::vector<std::string> cmds;
    int resets = 0;

    void attach(iTransportRxSink& s) { std::lock_guard<std::recursive_mutex> g(m_); sink_ = &s; }

    // Bytes the driver wrote.
    void hostWrite(const uint8_t* d, size_t n) {
        std::lock_guard<std::recursive_mutex> g(m_);
        for (size_t i = 0; i < n; ++i) {
            const uint8_t b = d[i];
            if (sendLeft_ > 0) {        // data after the ">" prompt
                links_[sendLink_].sent.push_back(b);
                if (--sendLeft_ == 0) out("\r\nRecv " + std::to_string(sendLen_) + " bytes\r\n\r\nSEND OK\r\n");
                continue;
            }
            if (silent) continue;
            if (echo_) out(std::string(1, static_cast<char>(b)));
            if (b == '\n') {
                std::string line = line_;
                line_.clear();
                while (!line.empty() && line.back() == '\r') line.pop_back();
                command(line);
            } else {
                line_ += static_cast<char>(b);
            }
        }
    }

    // ---- the network's side ----
    void peerSend(int l, const std::vector<uint8_t>& data) {
        std::lock_guard<std::recursive_mutex> g(m_);
        if (!links_[l].open) return;
        links_[l].rx.insert(links_[l].rx.end(), data.begin(), data.end());
        out("+IPD," + std::to_string(l) + "," + std::to_string(data.size()) + "\r\n");
    }
    int peerConnect(uint16_t port) {
        std::lock_guard<std::recursive_mutex> g(m_);
        if (!server_ || port != serverPort_) return -1;
        for (int l = 0; l < 5; ++l) {
            if (links_[l].open) continue;
            links_[l] = Link();
            links_[l].open = true;
            out(std::to_string(l) + ",CONNECT\r\n");
            return l;
        }
        return -1;
    }
    void peerClose(int l) {
        std::lock_guard<std::recursive_mutex> g(m_);
        if (!links_[l].open) return;
        links_[l].open = false;   // what it received stays readable, as in passive mode
        out(std::to_string(l) + ",CLOSED\r\n");
    }
    void dropWifi() {
        std::lock_guard<std::recursive_mutex> g(m_);
        closeAll();
        joined_ = false;
        out("WIFI DISCONNECT\r\n");
    }
    void reboot() { // a brown-out, say
        std::lock_guard<std::recursive_mutex> g(m_);
        powerOn();
        out("\r\n ets Jan  8 2013,rst cause:2, boot mode:(3,6)\r\n\r\nready\r\n");
    }

    // ---- inspection ----
    std::vector<uint8_t> sent(int l) { std::lock_guard<std::recursive_mutex> g(m_); return links_[l].sent; }
    bool linkOpen(int l) { std::lock_guard<std::recursive_mutex> g(m_); return links_[l].open; }
    bool joined() { std::lock_guard<std::recursive_mutex> g(m_); return joined_; }
    bool sawCmd(const std::string& c) {
        std::lock_guard<std::recursive_mutex> g(m_);
        for (auto& x : cmds) if (x == c) return true;
        return false;
    }
    std::string lastCmdStarting(const std::string& p) {
        std::lock_guard<std::recursive_mutex> g(m_);
        for (auto it = cmds.rbegin(); it != cmds.rend(); ++it) if (it->compare(0, p.size(), p) == 0) return *it;
        return "";
    }

private:
    struct Link { bool open = false; std::vector<uint8_t> rx, sent; };

    void out(const std::string& s) { for (char c : s) if (sink_) sink_->onByteReceived(static_cast<uint8_t>(c)); }
    void ok() { out("\r\nOK\r\n"); }
    void error() { out("\r\nERROR\r\n"); }
    static std::string ipStr(const IpAddress& a) {
        char b[20];
        std::snprintf(b, sizeof b, "%u.%u.%u.%u", a.b[0], a.b[1], a.b[2], a.b[3]);
        return b;
    }
    void closeAll() {
        for (int l = 0; l < 5; ++l) {
            if (!links_[l].open) continue;
            links_[l].open = false;
            out(std::to_string(l) + ",CLOSED\r\n");
        }
    }
    void powerOn() {
        for (auto& l : links_) l = Link();
        echo_ = true; joined_ = false; server_ = false; mux_ = false; passive_ = false;
        line_.clear();
        sendLeft_ = 0;
    }
    // The quoted, backslash-escaped fields of a command.
    static std::vector<std::string> quoted(const std::string& s) {
        std::vector<std::string> v;
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] != '"') continue;
            std::string f;
            for (++i; i < s.size() && s[i] != '"'; ++i) {
                if (s[i] == '\\' && i + 1 < s.size()) ++i;
                f += s[i];
            }
            v.push_back(f);
        }
        return v;
    }

    void command(const std::string& c) {
        if (c.empty()) return;
        cmds.push_back(c);
        if (busyNext > 0) { --busyNext; out("busy p...\r\n"); return; }
        int l = 0;
        unsigned n = 0;
        if (c == "AT" || c == "AT+CWMODE=1" || c == "AT+CWAUTOCONN=0" || c == "AT+CWDHCP=1,1" || c == "AT+CIPSTO=0") {
            ok();
        } else if (c == "AT+RST") {
            ok();
            ++resets;
            powerOn();
            out("\r\n ets Jan  8 2013,rst cause:2, boot mode:(3,6)\r\nload 0x40078000,len 13256\r\n\r\nready\r\n");
        } else if (c == "AT+GMR") {
            out("AT version:2.4.0.0(4c6eb5e - ESP32 - May 20 2022 03:12:58)\r\nSDK version:v4.2.2-dirty\r\n"
                "compile time(6118fc22):May 20 2022 11:03:44\r\nBin version:2.4.0(WROOM-32)\r\n");
            ok();
        } else if (c == "ATE0") {
            echo_ = false;
            ok();
        } else if (c == "AT+CIPMUX=1") {
            mux_ = true;
            ok();
        } else if (c == "AT+CIPRECVMODE=1") {
            passive_ = true;
            ok();
        } else if (c.compare(0, 10, "AT+CIPSTA=") == 0) {
            ok();
        } else if (c.compare(0, 9, "AT+CWJAP=") == 0) {
            const auto f = quoted(c);
            if (f.size() == 2 && f[0] == ssid && f[1] == pass) {
                joined_ = true;
                out("WIFI CONNECTED\r\nWIFI GOT IP\r\n");
                ok();
            } else {
                out("+CWJAP:1\r\n");
                error();
            }
        } else if (c == "AT+CWQAP") {
            if (joined_) { closeAll(); joined_ = false; out("WIFI DISCONNECT\r\n"); }
            ok();
        } else if (c == "AT+CIPSTA?") {
            const IpAddress none;
            out("+CIPSTA:ip:\"" + ipStr(joined_ ? ip : none) + "\"\r\n+CIPSTA:gateway:\"" + ipStr(joined_ ? gateway : none) +
                "\"\r\n+CIPSTA:netmask:\"" + ipStr(joined_ ? netmask : none) + "\"\r\n");
            ok();
        } else if (c == "AT+CIPDNS?") {
            out("+CIPDNS:1,\"" + ipStr(dns) + "\"\r\n");
            ok();
        } else if (c == "AT+CWJAP?") {
            if (joined_) out("+CWJAP:\"" + ssid + "\",\"aa:bb:cc:dd:ee:ff\",6,-61,0,1,3,0,1\r\n");
            else out("No AP\r\n");
            ok();
        } else if (std::sscanf(c.c_str(), "AT+CIPSTART=%d,", &l) == 1) {
            if (!joined_ || !mux_ || l < 0 || l > 4 || links_[l].open) { error(); return; }
            if (refuseConnect) { out(std::to_string(l) + ",CLOSED\r\n"); error(); return; }
            links_[l] = Link();
            links_[l].open = true;
            out(std::to_string(l) + ",CONNECT\r\n");
            ok();
        } else if (std::sscanf(c.c_str(), "AT+CIPSERVER=1,%u", &n) == 1) {
            server_ = true;
            serverPort_ = static_cast<uint16_t>(n);
            ok();
        } else if (std::sscanf(c.c_str(), "AT+CIPCLOSE=%d", &l) == 1) {
            if (l < 0 || l > 4 || !links_[l].open) { error(); return; }
            links_[l].open = false;
            out(std::to_string(l) + ",CLOSED\r\n");
            ok();
        } else if (std::sscanf(c.c_str(), "AT+CIPSEND=%d,%u", &l, &n) == 2) {
            if (l < 0 || l > 4 || !links_[l].open || n == 0 || n > 2048) { error(); return; }
            ok();
            out("\r\n>");
            sendLink_ = l;
            sendLeft_ = sendLen_ = n;
        } else if (std::sscanf(c.c_str(), "AT+CIPRECVDATA=%d,%u", &l, &n) == 2) {
            if (l < 0 || l > 4 || (!links_[l].open && links_[l].rx.empty())) { error(); return; }
            auto& rx = links_[l].rx;
            const size_t k = n < rx.size() ? n : rx.size();
            std::string payload(rx.begin(), rx.begin() + static_cast<long>(k));
            rx.erase(rx.begin(), rx.begin() + static_cast<long>(k));
            if (v1RecvFormat) out("+CIPRECVDATA," + std::to_string(k) + ":" + payload);
            else out("+CIPRECVDATA:" + std::to_string(k) + "," + payload);
            ok();
        } else {
            error();
        }
    }

    std::recursive_mutex m_;
    iTransportRxSink* sink_ = nullptr;
    std::string line_;
    bool echo_ = true, joined_ = false, server_ = false, mux_ = false, passive_ = false;
    uint16_t serverPort_ = 0;
    Link links_[5];
    int sendLink_ = 0;
    unsigned sendLeft_ = 0, sendLen_ = 0;
};

// iTransport straight onto SimEspAt: the UART.
class FakeEspUart : public iTransport {
public:
    explicit FakeEspUart(SimEspAt& m) : m_(m) {}
    bool write(const uint8_t* data, size_t len) override { m_.hostWrite(data, len); return true; }
    void setRxSink(iTransportRxSink& sink) override { m_.attach(sink); }
private:
    SimEspAt& m_;
};
