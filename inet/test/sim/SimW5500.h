// A simulated W5500 behind a simulated SPI transport, for the host
// tests. It decodes the real 3-byte frame header and follows the chip
// in the respects the driver depends on:
//
//   - MR.RST resets every register and reads back set for a few reads
//   - Sn_CR runs the command and reads back 0
//   - Sn_IR bits are cleared by writing 1s; SIR is computed from them
//   - Sn_TX_FSR / Sn_RX_RSR are computed from the buffer pointers, and
//     the buffers wrap at the size in Sn_TX/RXBUF_SIZE
//   - the 16-bit pointers start near 0xFFFF after OPEN (ptrStart), so
//     both the pointer and the buffer wrap get exercised
//
// The test plays the network with the peer*() calls. Thread-safe: the
// driver thread reaches it through FakeW5500Spi while a test thread
// plays the peer.
#pragma once
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <vector>
#include "iBlockTransport.h"
#include "NetTypes.h"
#include "W5500Regs.h"

class SimW5500 {
public:
    // ---- knobs ----
    enum class Connect { Accept, Refuse, Pending };
    uint8_t  version = W5500::w5500_version;
    bool     link = true;
    Connect  connectPolicy = Connect::Accept;
    bool     deferSendOk = false;   // SEND_OK only when the test says
    bool     closeCompletes = true; // DISCON reaches SOCK_CLOSED at once
    uint16_t ptrStart = 0xFFF0;
    std::function<void()> onIrq;    // the INT line fell
    // For FakeW5500Spi: the next transfer lands, failed, without
    // reaching the chip; or the bus won't take a transfer at all.
    bool     spiFailNext = false;
    bool     spiRefuse = false;
    // A UDP socket sent a datagram (called with no lock held).
    std::function<void(uint8_t s, IpAddress dst, uint16_t port, std::vector<uint8_t> data)> onUdpSend;

    // ---- what happened ----
    int resets = 0;
    int transfers = 0;

    SimW5500() { std::lock_guard<std::recursive_mutex> g(m_); hardReset(); }

    void access(bool write, const uint8_t* hdr, uint8_t* data, size_t len) {
        bool fire;
        std::vector<Datagram> out;
        {
            std::lock_guard<std::recursive_mutex> g(m_);
            ++transfers;
            const uint16_t addr = static_cast<uint16_t>((hdr[0] << 8) | hdr[1]);
            const uint8_t bsb = hdr[2] >> 3;
            const uint8_t before = sirLocked();
            if (write) writeBlock(bsb, addr, data, len);
            else       readBlock(bsb, addr, data, len);
            fire = sirLocked() & ~before;
            out.swap(udpOut_);
        }
        if (fire && onIrq) onIrq();
        for (auto& d : out) if (onUdpSend) onUdpSend(d.s, d.dst, d.port, d.data);
    }

    // ---- the network's side ----
    // A datagram arrives for UDP socket s.
    bool peerSendUdp(uint8_t s, const IpAddress& src, uint16_t srcPort, const uint8_t* data, size_t n) {
        bool ok = false;
        withIrq([&] {
            Sock& k = sock_[s];
            if (k.reg[W5500::w5500_Sn_SR] != W5500::w5500_SOCK_UDP) return;
            const size_t size = rxSize(s);
            if (size - static_cast<uint16_t>(k.rxWr - k.rxRd) < n + 8) return; // dropped, as on the chip
            const uint8_t hdr[8] = {src.b[0], src.b[1], src.b[2], src.b[3],
                                    uint8_t(srcPort >> 8), uint8_t(srcPort), uint8_t(n >> 8), uint8_t(n)};
            for (uint8_t b : hdr) k.rx[(k.rxWr++) & (size - 1)] = b;
            for (size_t i = 0; i < n; ++i) k.rx[(k.rxWr++) & (size - 1)] = data[i];
            k.ir |= W5500::w5500_IR_RECV;
            ok = true;
        });
        return ok;
    }

    size_t peerSend(uint8_t s, const uint8_t* data, size_t n) {
        size_t done = 0;
        withIrq([&] {
            Sock& k = sock_[s];
            if (k.reg[W5500::w5500_Sn_SR] != W5500::w5500_SOCK_ESTABLISHED) return;
            const size_t size = rxSize(s);
            const size_t space = size - static_cast<uint16_t>(k.rxWr - k.rxRd);
            done = n < space ? n : space;
            for (size_t i = 0; i < done; ++i) k.rx[(k.rxWr++) & (size - 1)] = data[i];
            if (done) k.ir |= W5500::w5500_IR_RECV;
        });
        return done;
    }
    void peerConnect(uint8_t s) {
        withIrq([&] {
            Sock& k = sock_[s];
            if (k.reg[W5500::w5500_Sn_SR] != W5500::w5500_SOCK_LISTEN) return;
            k.reg[W5500::w5500_Sn_SR] = W5500::w5500_SOCK_ESTABLISHED;
            k.ir |= W5500::w5500_IR_CON;
        });
    }
    void establish(uint8_t s) { // a Connect::Pending connection completes
        withIrq([&] {
            sock_[s].reg[W5500::w5500_Sn_SR] = W5500::w5500_SOCK_ESTABLISHED;
            sock_[s].ir |= W5500::w5500_IR_CON;
        });
    }
    void peerClose(uint8_t s) {
        withIrq([&] {
            Sock& k = sock_[s];
            if (k.reg[W5500::w5500_Sn_SR] != W5500::w5500_SOCK_ESTABLISHED) return;
            k.reg[W5500::w5500_Sn_SR] = W5500::w5500_SOCK_CLOSE_WAIT;
            k.ir |= W5500::w5500_IR_DISCON;
        });
    }
    void sendOk(uint8_t s) { withIrq([&] { sock_[s].ir |= W5500::w5500_IR_SENDOK; }); }

    // ---- inspection ----
    std::vector<uint8_t> sent(uint8_t s) { std::lock_guard<std::recursive_mutex> g(m_); return sock_[s].sent; }
    int sends(uint8_t s) { std::lock_guard<std::recursive_mutex> g(m_); return sock_[s].sends; }
    uint8_t status(uint8_t s) { std::lock_guard<std::recursive_mutex> g(m_); return sock_[s].reg[W5500::w5500_Sn_SR]; }
    uint8_t common(uint16_t a) { std::lock_guard<std::recursive_mutex> g(m_); return common_[a]; }
    uint8_t sockReg(uint8_t s, uint8_t a) { std::lock_guard<std::recursive_mutex> g(m_); refresh(s); return sock_[s].reg[a]; }
    uint16_t sockReg16(uint8_t s, uint8_t a) { return static_cast<uint16_t>((sockReg(s, a) << 8) | sockReg(s, a + 1)); }
    IpAddress connectedTo(uint8_t s) { std::lock_guard<std::recursive_mutex> g(m_); return sock_[s].dip; }
    uint16_t connectedPort(uint8_t s) { std::lock_guard<std::recursive_mutex> g(m_); return sock_[s].dport; }

private:
    struct Datagram { uint8_t s; IpAddress dst; uint16_t port; std::vector<uint8_t> data; };
    std::vector<Datagram> udpOut_;

    struct Sock {
        uint8_t  reg[0x30] = {0};
        uint8_t  ir = 0;
        uint8_t  tx[16384] = {0};
        uint8_t  rx[16384] = {0};
        uint16_t txRd = 0, txWr = 0, rxRd = 0, rxWr = 0;
        std::vector<uint8_t> sent;
        int      sends = 0;
        IpAddress dip;
        uint16_t dport = 0;
    };

    template <typename F> void withIrq(F f) {
        bool fire;
        {
            std::lock_guard<std::recursive_mutex> g(m_);
            const uint8_t before = sirLocked();
            f();
            fire = sirLocked() & ~before;
        }
        if (fire && onIrq) onIrq();
    }

    void hardReset() {
        std::memset(common_, 0, sizeof common_);
        common_[W5500::w5500_RTR] = 0x07; common_[W5500::w5500_RTR + 1] = 0xD0;
        common_[W5500::w5500_RCR] = 8;
        for (Sock& k : sock_) {
            std::memset(k.reg, 0, sizeof k.reg);
            k.reg[W5500::w5500_Sn_RXBUF_SIZE] = 2;
            k.reg[W5500::w5500_Sn_TXBUF_SIZE] = 2;
            k.reg[W5500::w5500_Sn_IMR] = 0xFF;
            k.ir = 0;
            k.txRd = k.txWr = k.rxRd = k.rxWr = 0;
        }
    }

    size_t rxSize(uint8_t s) const { return static_cast<size_t>(sock_[s].reg[W5500::w5500_Sn_RXBUF_SIZE]) * 1024; }
    size_t txSize(uint8_t s) const { return static_cast<size_t>(sock_[s].reg[W5500::w5500_Sn_TXBUF_SIZE]) * 1024; }

    uint8_t sirLocked() const {
        uint8_t sir = 0;
        for (uint8_t s = 0; s < 8; ++s) {
            if (sock_[s].ir & sock_[s].reg[W5500::w5500_Sn_IMR]) sir |= static_cast<uint8_t>(1u << s);
        }
        return sir & common_[W5500::w5500_SIMR];
    }

    static void put16(uint8_t* p, uint16_t v) { p[0] = static_cast<uint8_t>(v >> 8); p[1] = static_cast<uint8_t>(v); }

    // Computed registers, brought up to date before any access.
    void refresh(uint8_t s) {
        Sock& k = sock_[s];
        put16(k.reg + W5500::w5500_Sn_TX_FSR, static_cast<uint16_t>(txSize(s) - static_cast<uint16_t>(k.txWr - k.txRd)));
        put16(k.reg + W5500::w5500_Sn_TX_RD, k.txRd);
        put16(k.reg + W5500::w5500_Sn_TX_WR, k.txWr);
        put16(k.reg + W5500::w5500_Sn_RX_RSR, static_cast<uint16_t>(k.rxWr - k.rxRd));
        put16(k.reg + W5500::w5500_Sn_RX_RD, k.rxRd);
        put16(k.reg + W5500::w5500_Sn_RX_WR, k.rxWr);
        k.reg[W5500::w5500_Sn_IR] = k.ir;
    }

    void readBlock(uint8_t bsb, uint16_t addr, uint8_t* out, size_t len) {
        if (bsb == 0) {
            common_[W5500::w5500_SIR] = sirLocked();
            common_[W5500::w5500_VERSIONR] = version;
            common_[W5500::w5500_PHYCFGR] = link ? 0xBF : 0xB8;
            for (size_t i = 0; i < len; ++i) {
                const uint16_t a = static_cast<uint16_t>(addr + i);
                out[i] = a < sizeof common_ ? common_[a] : 0;
                if (a == W5500::w5500_MR && resetReadsLeft_ > 0 && --resetReadsLeft_ > 0) out[i] |= W5500::w5500_MR_RST;
            }
            return;
        }
        const uint8_t s = static_cast<uint8_t>((bsb - 1) / 4), kind = static_cast<uint8_t>((bsb - 1) % 4);
        Sock& k = sock_[s];
        if (kind == 0) {
            refresh(s);
            for (size_t i = 0; i < len; ++i) {
                const uint16_t a = static_cast<uint16_t>(addr + i);
                out[i] = a < sizeof k.reg ? k.reg[a] : 0;
            }
        } else {
            const size_t size = kind == 1 ? txSize(s) : rxSize(s);
            const uint8_t* mem = kind == 1 ? k.tx : k.rx;
            for (size_t i = 0; i < len; ++i) out[i] = size ? mem[(addr + i) & (size - 1)] : 0;
        }
    }

    void writeBlock(uint8_t bsb, uint16_t addr, const uint8_t* in, size_t len) {
        if (bsb == 0) {
            for (size_t i = 0; i < len; ++i) {
                const uint16_t a = static_cast<uint16_t>(addr + i);
                if (a == W5500::w5500_MR && (in[i] & W5500::w5500_MR_RST)) {
                    hardReset();
                    ++resets;
                    resetReadsLeft_ = 3; // reads back RST twice before it clears
                    continue;
                }
                if (a < sizeof common_) common_[a] = in[i];
            }
            return;
        }
        const uint8_t s = static_cast<uint8_t>((bsb - 1) / 4), kind = static_cast<uint8_t>((bsb - 1) % 4);
        Sock& k = sock_[s];
        if (kind == 0) {
            refresh(s);
            bool txWr = false, rxRd = false;
            for (size_t i = 0; i < len; ++i) {
                const uint16_t a = static_cast<uint16_t>(addr + i);
                if (a == W5500::w5500_Sn_CR) { command(s, in[i]); continue; }
                if (a == W5500::w5500_Sn_IR) { k.ir &= static_cast<uint8_t>(~in[i]); continue; }
                if (a == W5500::w5500_Sn_SR) continue; // read-only
                if (a == W5500::w5500_Sn_TX_WR || a == W5500::w5500_Sn_TX_WR + 1) txWr = true;
                if (a == W5500::w5500_Sn_RX_RD || a == W5500::w5500_Sn_RX_RD + 1) rxRd = true;
                if (a < sizeof k.reg) k.reg[a] = in[i];
            }
            if (txWr) k.txWr = static_cast<uint16_t>((k.reg[W5500::w5500_Sn_TX_WR] << 8) | k.reg[W5500::w5500_Sn_TX_WR + 1]);
            if (rxRd) k.rxRd = static_cast<uint16_t>((k.reg[W5500::w5500_Sn_RX_RD] << 8) | k.reg[W5500::w5500_Sn_RX_RD + 1]);
        } else {
            const size_t size = kind == 1 ? txSize(s) : rxSize(s);
            uint8_t* mem = kind == 1 ? k.tx : k.rx;
            for (size_t i = 0; i < len; ++i) if (size) mem[(addr + i) & (size - 1)] = in[i];
        }
    }

    void command(uint8_t s, uint8_t cmd) {
        Sock& k = sock_[s];
        uint8_t& sr = k.reg[W5500::w5500_Sn_SR];
        switch (cmd) {
        case W5500::w5500_CR_OPEN:
            if ((k.reg[W5500::w5500_Sn_MR] & 0x0F) == W5500::w5500_Sn_MR_TCP) sr = W5500::w5500_SOCK_INIT;
            if ((k.reg[W5500::w5500_Sn_MR] & 0x0F) == W5500::w5500_Sn_MR_UDP) sr = W5500::w5500_SOCK_UDP;
            k.txRd = k.txWr = k.rxRd = k.rxWr = ptrStart;
            k.sent.clear();
            k.sends = 0;
            break;
        case W5500::w5500_CR_CONNECT:
            if (sr != W5500::w5500_SOCK_INIT) break;
            std::memcpy(k.dip.b, k.reg + W5500::w5500_Sn_DIPR, 4);
            k.dport = static_cast<uint16_t>((k.reg[W5500::w5500_Sn_DPORT] << 8) | k.reg[W5500::w5500_Sn_DPORT + 1]);
            if (connectPolicy == Connect::Accept) { sr = W5500::w5500_SOCK_ESTABLISHED; k.ir |= W5500::w5500_IR_CON; }
            else if (connectPolicy == Connect::Refuse) { sr = W5500::w5500_SOCK_CLOSED; k.ir |= W5500::w5500_IR_TIMEOUT; }
            else sr = W5500::w5500_SOCK_SYNSENT;
            break;
        case W5500::w5500_CR_LISTEN:
            if (sr == W5500::w5500_SOCK_INIT) sr = W5500::w5500_SOCK_LISTEN;
            break;
        case W5500::w5500_CR_DISCON:
            if (sr == W5500::w5500_SOCK_ESTABLISHED) sr = closeCompletes ? W5500::w5500_SOCK_CLOSED : W5500::w5500_SOCK_FIN_WAIT;
            else if (sr == W5500::w5500_SOCK_CLOSE_WAIT) sr = closeCompletes ? W5500::w5500_SOCK_CLOSED : W5500::w5500_SOCK_LAST_ACK;
            if (closeCompletes) k.ir |= W5500::w5500_IR_DISCON;
            break;
        case W5500::w5500_CR_CLOSE:
            sr = W5500::w5500_SOCK_CLOSED;
            break;
        case W5500::w5500_CR_SEND: {
            const size_t size = txSize(s);
            if (sr == W5500::w5500_SOCK_UDP) {
                Datagram d;
                d.s = s;
                std::memcpy(d.dst.b, k.reg + W5500::w5500_Sn_DIPR, 4);
                d.port = static_cast<uint16_t>((k.reg[W5500::w5500_Sn_DPORT] << 8) | k.reg[W5500::w5500_Sn_DPORT + 1]);
                while (k.txRd != k.txWr) d.data.push_back(k.tx[(k.txRd++) & (size - 1)]);
                udpOut_.push_back(d);
                ++k.sends;
                k.ir |= W5500::w5500_IR_SENDOK;
                break;
            }
            while (k.txRd != k.txWr) k.sent.push_back(k.tx[(k.txRd++) & (size - 1)]);
            ++k.sends;
            if (!deferSendOk) k.ir |= W5500::w5500_IR_SENDOK;
            break;
        }
        default:
            break; // RECV: Sn_RX_RD has already been written
        }
        k.reg[W5500::w5500_Sn_CR] = 0;
    }

    std::recursive_mutex m_;
    uint8_t common_[0x40];
    Sock    sock_[8];
    int     resetReadsLeft_ = 0;
};

// iBlockTransport straight onto SimW5500. Every transfer lands at once.
class FakeW5500Spi : public iBlockTransport {
public:
    explicit FakeW5500Spi(SimW5500& chip) : chip_(chip) {}

    bool beginWrite(const uint8_t* hdr, uint8_t hl, const uint8_t* data, size_t len) override {
        return go(true, hdr, hl, const_cast<uint8_t*>(data), len);
    }
    bool beginRead(const uint8_t* hdr, uint8_t hl, uint8_t* data, size_t len) override {
        return go(false, hdr, hl, data, len);
    }
    bool isBusy() const override { return false; }
    bool lastOpFailed() const override { return failed_; }

private:
    bool go(bool write, const uint8_t* hdr, uint8_t hl, uint8_t* data, size_t len) {
        if (chip_.spiRefuse || hl != 3) return false;
        if (chip_.spiFailNext) { chip_.spiFailNext = false; failed_ = true; return true; }
        failed_ = false;
        chip_.access(write, hdr, data, len);
        return true;
    }
    SimW5500& chip_;
    bool failed_ = false;
};
