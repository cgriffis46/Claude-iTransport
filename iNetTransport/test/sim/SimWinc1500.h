// A simulated ATWINC1500 behind a simulated SPI transport, for the host
// tests. Byte by byte on the SPI side, as Microchip's host driver 19.5.2
// expects the module to answer (WincProtocol.h has the sources):
//
//   - commands C7 C8 C9 CA CF, with a CRC-7 byte while the CRC is on
//     (reg 0xE824 bits 2-3, on after a reset); a wrong CRC goes
//     unanswered; each command is echoed, then a state byte 00; a read's
//     data comes after an F3 byte, then two CRC bytes while it is on; a
//     block write's data follows an F3 byte, and is answered C3 00 (with
//     a byte before them while the CRC is off); CF FF FF FF resets the
//     parser and answers a skipped byte, CF, 00;
//   - the registers the boot handshake uses: efuses, the boot ROM's
//     magic, the host's version, the start word, "init done", the IRQ
//     enables, tstrGpRegs / tstrM2mRev / the MAC in data memory;
//   - the HIF: the request header in 0x108C, buffers handed out through
//     0x1078/0x150400 and taken back through 0x106C; the module's
//     messages through 0x1070/0x1084, one at a time, each waiting for
//     "RX done" (0x1070 bit 1) before the next; IRQN while one waits;
//   - the firmware: joins (access points with SSIDs and passphrases),
//     DHCP, RSSI, the system time, sockets (BIND, LISTEN, ACCEPT,
//     CONNECT, SEND, RECV, CLOSE) with the test playing the peers, and
//     DNS.
//
// What the real module does that isn't written down anywhere (timing,
// how many dummy bytes before an answer) is a guess, and kept loose:
// respLatency puts idle bytes before every answer, as the driver must
// tolerate: 0xFF before an echo, 0x00 before a read's F3 (Microchip's
// driver takes any byte 0xFx there as the data's start, so the module
// can't be sending 0xFF in that gap). Misuse by the host (a wrong CRC, memory outside a buffer,
// a second RECV on a socket, a header that doesn't match 0x108C...) is
// counted in misuse, which the tests expect to stay 0.
//
// Thread-safe: the driver's thread reaches it through FakeWincSpi while
// a test thread plays the network.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include "iBlockTransport.h"
#include "NetTypes.h"
#include "WincProtocol.h"

class SimWinc1500 {
public:
    // ---- knobs ----
    uint32_t chipIdRaw = 0x1003A0;       // ATWINC1500B (rev 3A0)
    uint8_t  fwMajor = 19, fwMinor = 6, fwPatch = 1;
    uint8_t  needMajor = 19, needMinor = 3, needPatch = 0;  // the least host driver it needs
    bool     oldFirmware = false;        // 19.3: no tstrGpRegs (0xC0008 reads 0)
    uint32_t efuseMs = 3, bootRomMs = 10, firmwareMs = 40;  // after reset / after the start word
    bool     firmwareHangs = false;      // never says "init done"
    bool     crcOffAtStart = false;      // the CRC left off from before (no reset since)
    int      respLatency = 1;            // idle bytes before each answer (0..5 tolerated)
    MacAddress mac = MacAddress(0xF8, 0xF0, 0x05, 0x01, 0x02, 0x03);
    int      allocBusyPolls = 0;         // reads of 0x1078 before a buffer is handed out (-1: never)
    uint16_t recvMax = 1400;             // most data in one RECV reply
    uint16_t recvDataOffset = 32;        // where a RECV reply's data starts, from the reply's start
    int8_t   rssi = -55;
    uint32_t joinMs = 300, dhcpMs = 200, connectMs = 30, dnsMs = 20;
    IpAddress dhcpIp = IpAddress(192, 168, 4, 23), dhcpGw = IpAddress(192, 168, 4, 1),
              dhcpDns = IpAddress(192, 168, 4, 1), dhcpMask = IpAddress(255, 255, 255, 0);
    uint32_t utcSeconds = 0;             // the module's clock (0: not synchronised)
    bool     peerCloseAborts = false;    // a peer's close reported as -12, not 0
    std::function<void()> onIrq;         // IRQN fell
    // For FakeWincSpi: the next transfer fails without reaching the
    // module; the bus refuses transfers; MISO stuck at this byte.
    bool     spiFailNext = false;
    bool     spiRefuse = false;
    int      stuckMiso = -1;
    int      corruptNextCommand = 0;     // flip a bit in the next N commands' CRC

    struct Ap { std::string ssid, pass; };
    std::vector<Ap> aps;
    std::map<std::string, IpAddress> dns;
    enum class Policy { Accept, Refuse, Silent };
    // Servers on the network: "a.b.c.d:port" -> what a connect gets.
    std::map<std::string, Policy> servers;

    // ---- what happened ----
    int misuse = 0;
    std::vector<std::string> misuseWhat;
    int commands = 0, crcErrors = 0, resets = 0, boots = 0, messagesIn = 0, messagesOut = 0;
    uint32_t hostVersion = 0, gp1 = 0;
    bool     irqEnabled() const { return (reg(0x1408) & (1u << 8)) && (reg(0x1A00) & (1u << 16)); }
    bool     running() const { std::lock_guard<std::recursive_mutex> g(m_); return state_ == Fw::Running; }
    bool     crcOn() const { std::lock_guard<std::recursive_mutex> g(m_); return (regs_[0xE824] & 0x0C) != 0; }
    int      joins = 0, sockOps = 0;
    std::string lastSsid, lastPass;
    uint8_t  lastSecType = 0, lastNoSave = 0;
    bool     dhcpOn = true;
    std::vector<uint8_t> staticIp;

    // A TCP connection, as the test sees it.
    struct Conn {
        int w = -1;                      // the module's socket
        IpAddress ip;
        uint16_t port = 0;
        bool incoming = false;
        bool deviceClosed = false;       // CLOSE from the host
        bool peerClosed = false;
        std::vector<uint8_t> fromDevice; // SENT by the host
        std::deque<uint8_t> toDevice;    // to come in RECV replies
    };
    std::vector<Conn> conns;

    SimWinc1500() { std::lock_guard<std::recursive_mutex> g(m_); powerOn(); }

    // ---- the pins ----
    void resetPin(bool asserted) {
        std::lock_guard<std::recursive_mutex> g(m_);
        inReset_ = asserted;
        if (!asserted) powerOn();
    }

    // ---- the network, played by the test ----
    int peerConnect(uint16_t port, IpAddress from = IpAddress(192, 168, 4, 77)) {
        std::lock_guard<std::recursive_mutex> g(m_);
        for (int w = 0; w < 7; ++w) {
            Sock& k = sk_[w];
            if (k.st != Sock::Listening || k.port != port) continue;
            int c = freeSock();
            if (c < 0) return -1;
            Sock& n = sk_[c];
            n = Sock();
            n.st = Sock::Connected;
            n.conn = static_cast<int>(conns.size());
            Conn cn;
            cn.w = c;
            cn.ip = from;
            cn.port = 50000 + static_cast<uint16_t>(conns.size());
            cn.incoming = true;
            conns.push_back(cn);
            std::vector<uint8_t> r(12, 0);
            WINC::put16(&r[0], 2);
            r[2] = static_cast<uint8_t>(cn.port >> 8); r[3] = static_cast<uint8_t>(cn.port);
            std::memcpy(&r[4], from.b, 4);
            r[8] = static_cast<uint8_t>(w);
            r[9] = static_cast<uint8_t>(c);
            WINC::put16(&r[10], 0x50);
            post(WINC::group_ip, WINC::sock_cmd_accept, r);
            return n.conn;
        }
        return -1;
    }
    void peerSend(int c, const uint8_t* d, size_t n) {
        std::lock_guard<std::recursive_mutex> g(m_);
        if (c < 0 || c >= static_cast<int>(conns.size())) return;
        conns[c].toDevice.insert(conns[c].toDevice.end(), d, d + n);
        pumpRecv();
    }
    void peerSend(int c, const std::string& s) { peerSend(c, reinterpret_cast<const uint8_t*>(s.data()), s.size()); }
    void peerClose(int c) {
        std::lock_guard<std::recursive_mutex> g(m_);
        if (c < 0 || c >= static_cast<int>(conns.size())) return;
        conns[c].peerClosed = true;
        pumpRecv();
    }
    std::vector<uint8_t> fromDevice(int c) {
        std::lock_guard<std::recursive_mutex> g(m_);
        return c >= 0 && c < static_cast<int>(conns.size()) ? conns[c].fromDevice : std::vector<uint8_t>();
    }
    Conn conn(int c) { std::lock_guard<std::recursive_mutex> g(m_); return conns.at(c); }
    int connCount() { std::lock_guard<std::recursive_mutex> g(m_); return static_cast<int>(conns.size()); }
    void apDrops() {   // the access point goes away
        std::lock_guard<std::recursive_mutex> g(m_);
        if (!assoc_) return;
        assoc_ = false;
        closeAllSockets();
        post(WINC::group_wifi, WINC::wifi_resp_con_state, {0, 0, 0, 0});
    }
    void dhcpRenew(IpAddress ip) {
        std::lock_guard<std::recursive_mutex> g(m_);
        dhcpIp = ip;
        sendDhcp();
    }
    // Sockets the firmware has open (connected, listening, bound).
    int openSockets() const {
        std::lock_guard<std::recursive_mutex> g(m_);
        int n = 0;
        for (const Sock& k : sk_) if (k.st != Sock::Free) ++n;
        return n;
    }
    bool msgWaiting() const { std::lock_guard<std::recursive_mutex> g(m_); return (regs_[0x1070] & 1) != 0 || awaitingRxDone_; }

    // ---- time ----
    void tick(uint32_t nowMs) {
        std::vector<std::function<void()>> fire;
        {
            std::lock_guard<std::recursive_mutex> g(m_);
            now_ = nowMs;
            boot();
            for (auto it = timers_.begin(); it != timers_.end();) {
                if (static_cast<int32_t>(now_ - it->first) >= 0) { fire.push_back(it->second); it = timers_.erase(it); }
                else ++it;
            }
        }
        for (auto& f : fire) { std::lock_guard<std::recursive_mutex> g(m_); f(); }
        std::lock_guard<std::recursive_mutex> g(m_);
        nextMessage();
    }

    // ---- the SPI, for FakeWincSpi ----
    void spiWrite(const uint8_t* d, size_t n) {
        std::lock_guard<std::recursive_mutex> g(m_);
        for (size_t i = 0; i < n; ++i) clock(d[i]);
    }
    void spiRead(uint8_t* d, size_t n) {
        std::lock_guard<std::recursive_mutex> g(m_);
        for (size_t i = 0; i < n; ++i) d[i] = clock(0x00);
        if (stuckMiso >= 0) for (size_t i = 0; i < n; ++i) d[i] = static_cast<uint8_t>(stuckMiso);
    }

    uint32_t reg(uint32_t a) const { std::lock_guard<std::recursive_mutex> g(m_); auto it = regs_.find(a); return it == regs_.end() ? 0 : it->second; }

private:
    enum class Fw { Off, Rom, Starting, Running };
    struct Sock {
        enum St { Free, Bound, Listening, Connecting, Connected };
        St st = Free;
        uint16_t port = 0;
        uint16_t session = 0;
        int conn = -1;
        bool recvPending = false;
        uint16_t recvSession = 0;
        bool closeReported = false;   // the peer's close went out in a RECV reply; CLOSE frees it
    };

    mutable std::recursive_mutex m_;
    mutable std::map<uint32_t, uint32_t> regs_;
    std::vector<uint8_t> dataMem_ = std::vector<uint8_t>(0x10000, 0);    // 0x30000..
    std::vector<uint8_t> shareMem_ = std::vector<uint8_t>(0x10000, 0);   // 0xD0000..
    uint32_t now_ = 0;
    bool inReset_ = false;
    Fw state_ = Fw::Off;
    uint32_t resetAt_ = 0, startAt_ = 0;
    std::multimap<uint32_t, std::function<void()>> timers_;
    // The SPI parser.
    std::deque<uint8_t> out_;
    std::vector<uint8_t> cmd_;
    size_t cmdNeed_ = 0;
    bool dataIn_ = false;
    bool dataToken_ = false;
    uint32_t dataAddr_ = 0;
    size_t dataLeft_ = 0, dataCrc_ = 0;
    // The HIF.
    int allocPolls_ = 0;
    bool allocated_ = false;
    uint32_t inBuf_ = 0xD0000;
    std::deque<std::pair<std::pair<uint8_t, uint8_t>, std::vector<uint8_t>>> outbox_;
    bool awaitingRxDone_ = false;
    uint32_t outBuf_ = 0xD8000;
    // The firmware.
    bool assoc_ = false;
    Sock sk_[7];

    void miss(const std::string& what) { ++misuse; misuseWhat.push_back(what); }
    void at(uint32_t ms, std::function<void()> f) { timers_.insert({now_ + ms, f}); }

    void powerOn() {
        regs_.clear();
        regs_[0xE824] = crcOffAtStart ? 0x20 : 0x2C;   // CRC on, 1 KB packets (bits 4-6 = 2)
        regs_[0x1000] = chipIdRaw;
        regs_[0x13F4] = 5;
        out_.clear();
        cmd_.clear();
        cmdNeed_ = 0;
        dataIn_ = false;
        state_ = Fw::Rom;
        resetAt_ = now_;
        timers_.clear();
        outbox_.clear();
        awaitingRxDone_ = false;
        allocated_ = false;
        assoc_ = false;
        for (Sock& k : sk_) k = Sock();
        ++resets;
    }
    void boot() {
        if (inReset_) return;
        if (state_ == Fw::Rom) {
            if (now_ - resetAt_ >= efuseMs) regs_[0x1014] |= 0x80000000u;
            if (now_ - resetAt_ >= bootRomMs && regs_[0xC000C] == 0) regs_[0xC000C] = WINC::finish_boot_rom;
        }
        if (state_ == Fw::Starting && !firmwareHangs && now_ - startAt_ >= firmwareMs) {
            state_ = Fw::Running;
            ++boots;
            regs_[0x108C] = WINC::finish_init_state;
            // tstrGpRegs at 0x8000, tstrM2mRev at 0x8100, the MAC at 0x8200.
            if (!oldFirmware) {
                regs_[0xC0008] = 0x8000;
                WINC::put32(&dataMem_[0x8000], 0x82000000u | 0x8200u);
                WINC::put32(&dataMem_[0x8004], 0x81000000u | 0x8100u);
                uint8_t* r = &dataMem_[0x8100];
                std::memset(r, 0, 40);
                WINC::put32(r, chipIdRaw);
                r[4] = fwMajor; r[5] = fwMinor; r[6] = fwPatch;
                r[7] = needMajor; r[8] = needMinor; r[9] = needPatch;
                std::memcpy(r + 10, "Jan  1 2019", 11);
                std::memcpy(&dataMem_[0x8200], mac.b, 6);
            }
        }
    }

    // ---- the SPI parser: one byte each way ----
    uint8_t clock(uint8_t in) {
        if (inReset_) return 0xFF;
        const uint8_t o = out_.empty() ? 0xFF : out_.front();
        if (!out_.empty()) out_.pop_front();
        const bool crc = (regs_[0xE824] & 0x0C) != 0;
        if (dataIn_) {   // a block write's data packet
            if (!dataToken_) {
                if ((in >> 4) == 0x0F) dataToken_ = true;   // F1..F3 (one packet here)
                return o;
            }
            if (dataLeft_) { writeMem(dataAddr_++, in); --dataLeft_; }
            else if (dataCrc_) --dataCrc_;
            if (dataLeft_ || dataCrc_) return o;
            dataIn_ = false;   // the packet's last byte: answer it
            if (!crc) answer({0xFF, WINC::data_write_ack, 0x00}, false);
            else answer({WINC::data_write_ack, 0x00}, false);
            return o;
        }
        if (cmdNeed_ == 0) {
            if (in < 0xC1 || in > 0xCF) return o;   // idle (the zeros a read clocks out)
            cmd_.assign(1, in);
            switch (in) {
            case WINC::cmd_single_read: cmdNeed_ = 4; break;
            case WINC::cmd_single_write: cmdNeed_ = 8; break;
            case WINC::cmd_dma_ext_read: case WINC::cmd_dma_ext_write: cmdNeed_ = 7; break;
            case WINC::cmd_reset: cmdNeed_ = 4; break;
            default: miss("unknown SPI command"); return o;
            }
            if (crc) ++cmdNeed_;
            out_.clear();
            return o;
        }
        cmd_.push_back(in);
        if (cmd_.size() < cmdNeed_) return o;
        cmdNeed_ = 0;
        execute(crc);
        return o;
    }
    void answer(std::vector<uint8_t> bytes, bool latency = true) {
        if (latency) for (int i = 0; i < respLatency; ++i) out_.push_back(0xFF);
        for (uint8_t b : bytes) out_.push_back(b);
    }
    void execute(bool crc) {
        ++commands;
        const uint8_t c = cmd_[0];
        if (crc) {
            const uint8_t want = static_cast<uint8_t>(WINC::crc7(0x7F, cmd_.data(), cmd_.size() - 1) << 1);
            uint8_t got = cmd_.back();
            if (corruptNextCommand > 0) { --corruptNextCommand; got ^= 0x02; }
            if (got != want) { ++crcErrors; return; }   // unanswered
        } else if (corruptNextCommand > 0) {
            --corruptNextCommand;
            return;   // garbled in some other way: unanswered
        }
        const uint32_t a = (static_cast<uint32_t>(cmd_[1]) << 16) | (cmd_[2] << 8) | cmd_[3];
        switch (c) {
        case WINC::cmd_reset:
            out_.clear();
            out_.push_back(0xFF);   // the byte the driver skips
            answer({c, 0x00});
            return;
        case WINC::cmd_single_read: {
            const uint32_t v = readReg(a);
            std::vector<uint8_t> r = {c, 0x00};
            for (int i = 0; i < respLatency; ++i) r.push_back(0x00);   // not 0xFx: that would read as the data's start
            r.push_back(0xF3);
            for (int i = 0; i < 4; ++i) r.push_back(static_cast<uint8_t>(v >> (8 * i)));
            if (crc) { r.push_back(0x5A); r.push_back(0xA5); }
            answer(r);
            return;
        }
        case WINC::cmd_single_write: {
            const uint32_t v = (static_cast<uint32_t>(cmd_[4]) << 24) | (cmd_[5] << 16) | (cmd_[6] << 8) | cmd_[7];
            answer({c, 0x00});
            writeReg(a, v);
            return;
        }
        case WINC::cmd_dma_ext_read: {
            const size_t n = (static_cast<size_t>(cmd_[4]) << 16) | (cmd_[5] << 8) | cmd_[6];
            std::vector<uint8_t> r = {c, 0x00};
            for (int i = 0; i < respLatency; ++i) r.push_back(0x00);
            r.push_back(0xF3);
            for (size_t i = 0; i < n; ++i) r.push_back(readMem(a + static_cast<uint32_t>(i)));
            if (crc) { r.push_back(0x5A); r.push_back(0xA5); }
            answer(r);
            return;
        }
        case WINC::cmd_dma_ext_write: {
            const size_t n = (static_cast<size_t>(cmd_[4]) << 16) | (cmd_[5] << 8) | cmd_[6];
            answer({c, 0x00});
            dataIn_ = true;
            dataToken_ = false;
            dataAddr_ = a;
            dataLeft_ = n;
            dataCrc_ = crc ? 2 : 0;
            return;
        }
        }
    }

    // ---- memory ----
    uint8_t* mem(uint32_t a) {
        if (a >= 0x30000 && a < 0x40000) return &dataMem_[a - 0x30000];
        if (a >= 0xD0000 && a < 0xE0000) return &shareMem_[a - 0xD0000];
        return nullptr;
    }
    uint8_t readMem(uint32_t a) {
        if (awaitingRxDone_ && a >= outBuf_ && a < outBuf_ + 0x800) {
            if (a >= outBuf_ + outLen_) miss("read past the module's message");
        }
        uint8_t* p = mem(a);
        if (!p) { miss("read outside memory"); return 0xFF; }
        return *p;
    }
    void writeMem(uint32_t a, uint8_t v) {
        if (!(allocated_ && a >= inBuf_ && a < inBuf_ + WINC::hif_max_bytes)) miss("write outside the buffer handed out");
        uint8_t* p = mem(a);
        if (p) *p = v;
    }
    uint32_t outLen_ = 0;

    // ---- registers ----
    uint32_t readReg(uint32_t a) {
        if (a == 0x1078) {   // a buffer: bit 1 clears once there is one
            uint32_t v = regs_[a];
            if ((v & 2) && !allocated_) {
                if (state_ != Fw::Running || allocBusyPolls < 0 || allocPolls_ < allocBusyPolls) { ++allocPolls_; return v; }
                allocated_ = true;
                regs_[0x150400] = inBuf_;
                regs_[a] = v & ~2u;
                return regs_[a];
            }
            return v;
        }
        if (a == 0x150400 && !allocated_) miss("buffer address read before one was handed out");
        return regs_[a];
    }
    void writeReg(uint32_t a, uint32_t v) {
        switch (a) {
        case 0x1400:   // a global reset
            if (v == 0) {   // after answering the write that asked for it
                const std::deque<uint8_t> answerKept = out_;
                const bool keep = crcOffAtStart;
                crcOffAtStart = false;
                powerOn();
                crcOffAtStart = keep;
                out_ = answerKept;
            }
            return;
        case 0x108C:
            if (state_ == Fw::Rom) { hostVersion = v; regs_[a] = v; return; }
            regs_[a] = v;
            return;
        case 0x14A0: gp1 = v; regs_[a] = v; return;
        case 0xC000C:
            if (v == WINC::start_firmware) {
                if (state_ != Fw::Rom || regs_[0xC000C] != WINC::finish_boot_rom) miss("firmware started before the boot ROM was ready");
                if (hostVersion != WINC::host_version_info) miss("the host didn't report its version");
                state_ = Fw::Starting;
                startAt_ = now_;
            }
            regs_[a] = v;
            return;
        case 0x1078:
            if (v & 2) {
                if (state_ != Fw::Running) miss("a buffer asked for before the firmware runs");
                allocated_ = false;
                allocPolls_ = 0;
            }
            regs_[a] = v;
            return;
        case 0x106C:
            regs_[a] = v;
            if (v & 2) takeMessage(v >> 2);
            return;
        case 0x1070: {
            const uint32_t old = regs_[a];
            if ((old & 1) && !(v & 1)) {}   // the host took the interrupt
            if ((v & 1) && !(old & 1)) miss("0x1070 bit 0 set by the host");
            regs_[a] = v & ~2u;
            if (v & 2) {
                if (!awaitingRxDone_) miss("RX done with no message");
                awaitingRxDone_ = false;
                regs_[a] = 0;
                nextMessage();
            }
            return;
        }
        default:
            regs_[a] = v;
        }
    }

    // ---- the HIF ----
    void takeMessage(uint32_t addr) {
        if (!allocated_ || addr != inBuf_) { miss("a message at an address not handed out"); return; }
        allocated_ = false;
        const uint8_t* p = mem(addr);
        const uint32_t hdr = regs_[0x108C];
        const uint8_t gid = p[0], op = p[1];
        const uint16_t len = WINC::le16(p + 2);
        if ((hdr & 0xFF) != gid || ((hdr >> 8) & 0x7F) != op || (hdr >> 16) != len) miss("header differs from 0x108C");
        if (len < 8 || len > WINC::hif_max_bytes) { miss("message length"); return; }
        ++messagesIn;
        const bool data = ((hdr >> 8) & WINC::req_data_pkt) != 0;
        std::vector<uint8_t> body(p + 8, p + len);
        request(gid, op, data, body);
    }
    void post(uint8_t gid, uint8_t op, const std::vector<uint8_t>& payload) {
        outbox_.push_back({{gid, op}, payload});
        nextMessage();
    }
    void nextMessage() {
        if (awaitingRxDone_ || outbox_.empty() || state_ != Fw::Running) return;
        auto m = outbox_.front();
        outbox_.pop_front();
        const uint16_t len = static_cast<uint16_t>(8 + m.second.size());
        uint8_t* p = mem(outBuf_);
        std::memset(p, 0xEE, 0x800);
        p[0] = m.first.first; p[1] = m.first.second; WINC::put16(p + 2, len); p[4] = p[5] = p[6] = p[7] = 0;
        std::memcpy(p + 8, m.second.data(), m.second.size());
        outLen_ = len;
        awaitingRxDone_ = true;
        regs_[0x1084] = outBuf_;
        regs_[0x1070] = (static_cast<uint32_t>(len) << 2) | 1u;
        ++messagesOut;
        if (irqEnabled() && onIrq) onIrq();
    }

    // ---- the firmware ----
    int freeSock() { for (int w = 0; w < 7; ++w) if (sk_[w].st == Sock::Free) return w; return -1; }
    void closeAllSockets() {
        for (Sock& k : sk_) {
            if (k.conn >= 0) conns[k.conn].peerClosed = true;
            k = Sock();
        }
    }
    void sendDhcp() {
        std::vector<uint8_t> c(20, 0);
        std::memcpy(&c[0], dhcpIp.b, 4); std::memcpy(&c[4], dhcpGw.b, 4);
        std::memcpy(&c[8], dhcpDns.b, 4); std::memcpy(&c[12], dhcpMask.b, 4);
        WINC::put32(&c[16], 3600);
        post(WINC::group_wifi, WINC::wifi_req_dhcp_conf, c);
    }
    void request(uint8_t gid, uint8_t op, bool data, const std::vector<uint8_t>& b) {
        if (gid == WINC::group_wifi) wifi(op, b);
        else if (gid == WINC::group_ip) ip(op, data, b);
        else miss("unknown group");
    }
    void wifi(uint8_t op, const std::vector<uint8_t>& b) {
        switch (op) {
        case WINC::wifi_req_connect: {
            if (b.size() != WINC::connect_bytes) { miss("CONNECT size"); return; }
            ++joins;
            lastSsid = std::string(reinterpret_cast<const char*>(&b[70]));
            lastPass = std::string(reinterpret_cast<const char*>(&b[0]));
            lastSecType = b[65];
            lastNoSave = b[103];
            if (WINC::le16(&b[68]) != WINC::channel_all) miss("channel");
            at(joinMs, [this]() {
                for (const Ap& ap : aps) {
                    if (ap.ssid != lastSsid) continue;
                    const bool ok = ap.pass.empty() ? lastSecType == WINC::sec_open
                                                    : (lastSecType == WINC::sec_wpa_psk && ap.pass == lastPass);
                    if (!ok) { post(WINC::group_wifi, WINC::wifi_resp_con_state, {0, WINC::err_auth_fail, 0, 0}); return; }
                    assoc_ = true;
                    post(WINC::group_wifi, WINC::wifi_resp_con_state, {1, 0, 0, 0});
                    if (dhcpOn) at(dhcpMs, [this]() { if (assoc_) sendDhcp(); });
                    return;
                }
                post(WINC::group_wifi, WINC::wifi_resp_con_state, {0, WINC::err_scan_fail, 0, 0});
            });
            return;
        }
        case WINC::wifi_req_disconnect:
            at(5, [this]() {
                const bool was = assoc_;
                assoc_ = false;
                closeAllSockets();
                (void)was;
                post(WINC::group_wifi, WINC::wifi_resp_con_state, {0, 0, 0, 0});
            });
            return;
        case WINC::wifi_req_current_rssi:
            post(WINC::group_wifi, WINC::wifi_resp_current_rssi, {static_cast<uint8_t>(rssi), 0, 0, 0});
            return;
        case WINC::wifi_req_get_sys_time: {
            std::vector<uint8_t> t(8, 0);
            if (utcSeconds) {
                int64_t z = utcSeconds / 86400 + 719468;   // civil_from_days
                const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
                const unsigned doe = static_cast<unsigned>(z - era * 146097);
                const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
                const int64_t y = static_cast<int64_t>(yoe) + era * 400;
                const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
                const unsigned mp = (5 * doy + 2) / 153;
                const unsigned d = doy - (153 * mp + 2) / 5 + 1;
                const unsigned m = mp < 10 ? mp + 3 : mp - 9;
                const uint32_t sod = utcSeconds % 86400;
                WINC::put16(&t[0], static_cast<uint16_t>(y + (m <= 2)));
                t[2] = static_cast<uint8_t>(m); t[3] = static_cast<uint8_t>(d);
                t[4] = static_cast<uint8_t>(sod / 3600); t[5] = static_cast<uint8_t>(sod / 60 % 60); t[6] = static_cast<uint8_t>(sod % 60);
            }
            post(WINC::group_wifi, WINC::wifi_resp_get_sys_time, t);
            return;
        }
        case WINC::wifi_req_enable_sntp:
            return;
        default:
            miss("unknown Wi-Fi request");
        }
    }
    void ip(uint8_t op, bool data, const std::vector<uint8_t>& b) {
        ++sockOps;
        auto sockOk = [&](int w) { if (w < 0 || w >= 7) { miss("socket number"); return false; } return true; };
        switch (op) {
        case WINC::ip_req_enable_dhcp: dhcpOn = true; return;
        case WINC::ip_req_disable_dhcp: dhcpOn = false; return;
        case WINC::ip_req_static_ip_conf:
            if (b.size() != WINC::ip_config_bytes) miss("static IP size");
            staticIp = b;
            return;
        case WINC::sock_cmd_bind: {
            if (b.size() != WINC::bind_bytes) { miss("BIND size"); return; }
            const int w = static_cast<int8_t>(b[8]);
            if (!sockOk(w)) return;
            const uint16_t port = static_cast<uint16_t>((b[2] << 8) | b[3]);
            int8_t st = 0;
            if (WINC::le16(&b[0]) != 2) miss("BIND family");
            for (const Sock& k : sk_) if (k.st != Sock::Free && k.port == port && (k.st == Sock::Bound || k.st == Sock::Listening)) st = WINC::sock_err_addr_in_use;
            if (sk_[w].st != Sock::Free) { miss("BIND on a socket in use"); st = -9; }
            if (st == 0) { sk_[w] = Sock(); sk_[w].st = Sock::Bound; sk_[w].port = port; sk_[w].session = WINC::le16(&b[10]); }
            std::vector<uint8_t> r = {static_cast<uint8_t>(w), static_cast<uint8_t>(st), b[10], b[11]};
            post(WINC::group_ip, WINC::sock_cmd_bind, r);
            return;
        }
        case WINC::sock_cmd_listen: {
            if (b.size() != WINC::listen_bytes) { miss("LISTEN size"); return; }
            const int w = static_cast<int8_t>(b[0]);
            if (!sockOk(w)) return;
            int8_t st = 0;
            if (sk_[w].st != Sock::Bound) { miss("LISTEN on a socket not bound"); st = -9; }
            else sk_[w].st = Sock::Listening;
            post(WINC::group_ip, WINC::sock_cmd_listen, {static_cast<uint8_t>(w), static_cast<uint8_t>(st), b[2], b[3]});
            return;
        }
        case WINC::sock_cmd_connect: {
            if (b.size() != WINC::connect_cmd_bytes) { miss("CONNECT size"); return; }
            const int w = static_cast<int8_t>(b[8]);
            if (!sockOk(w)) return;
            if (sk_[w].st != Sock::Free) { miss("CONNECT on a socket in use"); return; }
            if (!assoc_) miss("CONNECT while not joined");
            const uint16_t port = static_cast<uint16_t>((b[2] << 8) | b[3]);
            const IpAddress ip(b[4], b[5], b[6], b[7]);
            char key[32];
            std::snprintf(key, sizeof key, "%u.%u.%u.%u:%u", ip.b[0], ip.b[1], ip.b[2], ip.b[3], port);
            sk_[w] = Sock();
            sk_[w].st = Sock::Connecting;
            sk_[w].session = WINC::le16(&b[10]);
            auto it = servers.find(key);
            const Policy pol = it == servers.end() ? Policy::Refuse : it->second;
            if (pol == Policy::Silent) return;
            at(connectMs, [this, w, ip, port, pol]() {
                if (sk_[w].st != Sock::Connecting) return;
                if (pol == Policy::Refuse) {
                    sk_[w] = Sock();
                    post(WINC::group_ip, WINC::sock_cmd_connect, {static_cast<uint8_t>(w), static_cast<uint8_t>(WINC::sock_err_conn_aborted), 0, 0});
                    return;
                }
                sk_[w].st = Sock::Connected;
                sk_[w].conn = static_cast<int>(conns.size());
                Conn c;
                c.w = w; c.ip = ip; c.port = port;
                conns.push_back(c);
                post(WINC::group_ip, WINC::sock_cmd_connect, {static_cast<uint8_t>(w), 0, 0x50, 0});
            });
            return;
        }
        case WINC::sock_cmd_send: {
            if (!data) miss("SEND without the data bit");
            if (b.size() < WINC::send_cmd_bytes) { miss("SEND size"); return; }
            const int w = static_cast<int8_t>(b[0]);
            if (!sockOk(w)) return;
            const uint16_t n = WINC::le16(&b[2]);
            if (n > WINC::socket_max_send) miss("SEND over 1400 bytes");
            if (b.size() != static_cast<size_t>(WINC::send_data_offset + n)) miss("SEND data not at offset 80");
            int16_t sent = static_cast<int16_t>(n);
            Sock& k = sk_[w];
            // The session isn't checked, only echoed: the host numbers an
            // accepted connection's session itself, and the firmware never
            // hears that number until the first request on it.
            if (k.st != Sock::Connected || k.closeReported) sent = WINC::sock_err_conn_aborted;
            else if (b.size() >= static_cast<size_t>(WINC::send_data_offset + n)) {
                Conn& c = conns[k.conn];
                c.fromDevice.insert(c.fromDevice.end(), b.begin() + WINC::send_data_offset, b.begin() + WINC::send_data_offset + n);
            }
            std::vector<uint8_t> r(8, 0);
            r[0] = static_cast<uint8_t>(w);
            WINC::put16(&r[2], static_cast<uint16_t>(sent));
            r[4] = b[12]; r[5] = b[13];
            post(WINC::group_ip, WINC::sock_cmd_send, r);
            return;
        }
        case WINC::sock_cmd_recv: {
            if (b.size() != WINC::recv_cmd_bytes) { miss("RECV size"); return; }
            const int w = static_cast<int8_t>(b[4]);
            if (!sockOk(w)) return;
            Sock& k = sk_[w];
            if (k.st != Sock::Connected) { miss("RECV on a socket not connected"); return; }
            if (k.recvPending) miss("a second RECV on a socket");
            k.recvPending = true;
            k.recvSession = WINC::le16(&b[6]);
            pumpRecv();
            return;
        }
        case WINC::sock_cmd_close: {
            if (b.size() != WINC::close_bytes) { miss("CLOSE size"); return; }
            const int w = static_cast<int8_t>(b[0]);
            if (!sockOk(w)) return;
            Sock& k = sk_[w];
            if (k.conn >= 0) conns[k.conn].deviceClosed = true;
            k = Sock();
            return;
        }
        case WINC::sock_cmd_dns_resolve: {
            const std::string name(reinterpret_cast<const char*>(b.data()), strnlen(reinterpret_cast<const char*>(b.data()), b.size()));
            if (b.empty() || b.back() != 0 || name.size() + 1 != b.size()) miss("DNS name not NUL-terminated");
            at(dnsMs, [this, name]() {
                std::vector<uint8_t> r(68, 0);
                std::memcpy(&r[0], name.data(), name.size() < 63 ? name.size() : 63);
                auto it = dns.find(name);
                if (it != dns.end()) std::memcpy(&r[64], it->second.b, 4);
                post(WINC::group_ip, WINC::sock_cmd_dns_resolve, r);
            });
            return;
        }
        default:
            miss("unknown IP request");
        }
    }
    // RECV replies for the sockets waiting with one, when there's data or
    // the peer has closed.
    void pumpRecv() {
        for (int w = 0; w < 7; ++w) {
            Sock& k = sk_[w];
            if (k.st != Sock::Connected || !k.recvPending || k.conn < 0) continue;
            Conn& c = conns[k.conn];
            if (c.toDevice.empty() && !c.peerClosed && !k.closeReported) continue;
            k.recvPending = false;
            const size_t n = c.toDevice.size() < recvMax ? c.toDevice.size() : recvMax;
            std::vector<uint8_t> r(n ? recvDataOffset + n : 16, 0);
            WINC::put16(&r[0], 2);
            r[2] = static_cast<uint8_t>(c.port >> 8); r[3] = static_cast<uint8_t>(c.port);
            std::memcpy(&r[4], c.ip.b, 4);
            const int16_t status = n ? static_cast<int16_t>(n) : (peerCloseAborts ? WINC::sock_err_conn_aborted : 0);
            WINC::put16(&r[8], static_cast<uint16_t>(status));
            WINC::put16(&r[10], n ? recvDataOffset : 0);
            r[12] = static_cast<uint8_t>(w);
            WINC::put16(&r[14], k.recvSession);
            for (size_t i = 0; i < n; ++i) { r[recvDataOffset + i] = c.toDevice.front(); c.toDevice.pop_front(); }
            if (!n) k.closeReported = true;   // the socket stays until the host's CLOSE
            post(WINC::group_ip, WINC::sock_cmd_recv, r);
        }
    }
};

// The SPI bus to it, as the driver sees it: each transfer is one
// chip-select, done at once.
class FakeWincSpi : public iBlockTransport {
public:
    explicit FakeWincSpi(SimWinc1500& sim) : sim_(sim) {}
    int transfers = 0;
    bool beginWrite(const uint8_t* hdr, uint8_t hl, const uint8_t* data, size_t len) override {
        if (!start()) return false;
        if (!failed_) { if (hl) sim_.spiWrite(hdr, hl); if (len) sim_.spiWrite(data, len); }
        return true;
    }
    bool beginRead(const uint8_t* hdr, uint8_t hl, uint8_t* data, size_t len) override {
        if (!start()) return false;
        if (hl) sim_.spiWrite(hdr, hl);
        if (failed_) { std::memset(data, 0xFF, len); return true; }
        sim_.spiRead(data, len);
        return true;
    }
    bool isBusy() const override { return false; }
    bool lastOpFailed() const override { return failed_; }

private:
    bool start() {
        if (sim_.spiRefuse) return false;
        ++transfers;
        failed_ = sim_.spiFailNext;
        sim_.spiFailNext = false;
        return true;
    }
    SimWinc1500& sim_;
    bool failed_ = false;
};
