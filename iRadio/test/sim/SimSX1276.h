/*
 * SimSX1276.h
 *
 * For the iRadio host tests: SX1276 radios (RFM95) at register level in
 * LoRa mode, sharing an air, and two transports to reach one through (a
 * bare ISensorTransport, and itransport's real SPITransport with the HAL
 * faked, which checks the write-high address bit).
 *
 * A transmission takes its time on air (LoRaPhy's formula). Another
 * radio receives it if it was in RX when the packet began, or entered RX
 * while enough of the preamble was left to lock on (kLockSymbols of the
 * preamble's 8 + 4.25 symbols: an approximation of the SX1276, the same
 * minimum LoRaMac-node assumes), and stays in RX without retuning until it ends, with the same
 * frequency, bandwidth, spreading factor, low data rate setting and sync
 * word, and the matching I/Q polarity (the transmitter's TX inversion is
 * the receiver's RX inversion, with RegInvertIq2 set to match). An RX
 * single gives up (RxTimeout) when no packet has begun within its symbol
 * timeout. So a driver that writes a setting wrongly, or tunes late,
 * misses the packet, as it would on the air.
 */
#pragma once
#include <cstdint>
#include <cstring>
#include <functional>
#include <vector>
#include "ISensorTransport.h"
#include "SPITransport.h"
#include "SX1276Regs.h"
#include "LoRaPhy.h"

namespace sim {

struct Air;

struct Sx1276 {
	uint8_t reg[0x80];
	uint8_t fifo[256];
	bool present = true;        // answers at all
	bool stuck = false;         // transfers never complete
	bool refuse = false;        // the transport cannot start a transfer
	bool txBroken = false;      // TX never finishes (a dead PA or a reset chip)
	uint8_t version = sx1276::kVersion;
	uint8_t rssiRaw = 100;      // what a received packet reads: -157 + 100 + 6 = -51 dBm
	uint8_t snrRaw = 40;        // 10 dB
	int corruptNext = 0;        // the next n packets received fail their CRC
	uint32_t now = 0;
	std::function<void()> dio0, dio1;
	Air* air = nullptr;

	// What happened.
	struct Sent { std::vector<uint8_t> data; uint32_t freqHz; uint8_t sf, bw, paConfig, paDac; bool invertIq, crc; uint32_t start, end; };
	std::vector<Sent> sent;
	int received = 0, timeouts = 0, writes = 0;

	// State.
	uint32_t gen = 0;           // counts mode and tuning changes
	uint32_t rxSince = 0;
	uint8_t rxWriteAddr = 0;    // where RX continuous puts the next packet
	bool txOn = false;
	uint32_t txEnd = 0;
	struct Incoming { bool on = false; uint32_t gen = 0; bool collided = false; } incoming;

	Sx1276() {
		std::memset(reg, 0, sizeof reg);
		std::memset(fifo, 0, sizeof fifo);
		reg[sx1276::kRegOpMode] = 0x09;         // FSK, low frequency mode, standby (reset value)
		reg[sx1276::kRegInvertIq] = 0x27;
		reg[sx1276::kRegInvertIq2] = 0x1D;
		reg[sx1276::kRegSyncWord] = 0x12;
		reg[sx1276::kRegModemConfig1] = 0x72;
		reg[sx1276::kRegModemConfig2] = 0x70;
	}

	uint8_t mode() const { return reg[sx1276::kRegOpMode] & sx1276::kOpModeMask; }
	bool lora() const { return reg[sx1276::kRegOpMode] & sx1276::kOpLoRa; }
	bool inRx() const { return lora() && (mode() == sx1276::kOpRxSingle || mode() == sx1276::kOpRxContinuous); }
	uint32_t freqHz() const {
		return lora::freqFromFrf(((uint32_t)reg[0x06] << 16) | ((uint32_t)reg[0x07] << 8) | reg[0x08]);
	}
	uint8_t sf() const { return reg[sx1276::kRegModemConfig2] >> 4; }
	uint8_t bw() const { return reg[sx1276::kRegModemConfig1] >> 4; }
	bool ldro() const { return reg[sx1276::kRegModemConfig3] & sx1276::kLdro; }
	bool crcOn() const { return reg[sx1276::kRegModemConfig2] & sx1276::kCrcOn; }
	bool txInverted() const { return !(reg[sx1276::kRegInvertIq] & 0x01); }
	bool rxInverted() const { return reg[sx1276::kRegInvertIq] & 0x40; }
	uint16_t symbTimeout() const { return (uint16_t)(((reg[sx1276::kRegModemConfig2] & 0x03) << 8) | reg[sx1276::kRegSymbTimeoutLsb]); }

	void write(uint8_t r, uint8_t v);
	uint8_t read(uint8_t r) {
		if (r == sx1276::kRegFifo) return fifo[reg[sx1276::kRegFifoAddrPtr]++];
		if (r == sx1276::kRegVersion) return version;
		return reg[r & 0x7F];
	}
	void tick(uint32_t t);
	void deliver(const Sent& p);   // the end of a packet this radio locked onto
};

// The air: every radio, and the packets on it.
struct Air {
	std::vector<Sx1276*> radios;
	struct OnAir { Sx1276* from; Sx1276::Sent p; std::vector<Sx1276*> listeners; };
	std::vector<OnAir> flying;
	// A gateway: hears every packet whole when it ends, whatever its
	// channel or spreading factor (it filters for itself), collisions aside.
	std::function<void(const Sx1276& from, const Sx1276::Sent& p)> sniff;

	void add(Sx1276& r) { radios.push_back(&r); r.air = this; }

	static constexpr uint32_t kLockSymbols = 6;   // of preamble a receiver needs to lock on
	// A radio just entered RX: a packet already under way can still be
	// caught while kLockSymbols of its preamble (programmed + 4.25) are left.
	void catchUp(Sx1276& r) {
		for (OnAir& a : flying) {
			if (a.from == &r || !matches(*a.from, a.p, r) || r.incoming.on) continue;
			const uint32_t symUs = lora::symbolUs(a.p.sf, (lora::Bw)a.p.bw);
			const uint32_t lateUs = (uint32_t)((uint64_t)symUs * (8 * 4 + 17 - 4 * kLockSymbols) / 4);   // (8 + 4.25 - 6) symbols
			if ((r.now - a.p.start) * 1000u > lateUs) continue;
			r.incoming.on = true;
			r.incoming.gen = r.gen;
			r.incoming.collided = false;
			a.listeners.push_back(&r);
		}
	}

	bool matches(const Sx1276& tx, const Sx1276::Sent& p, const Sx1276& rx) const {
		return rx.inRx() && rx.freqHz() == p.freqHz && rx.sf() == p.sf && rx.bw() == p.bw &&
		       rx.ldro() == tx.ldro() && rx.reg[sx1276::kRegSyncWord] == tx.reg[sx1276::kRegSyncWord] &&
		       rx.rxInverted() == p.invertIq &&
		       rx.reg[sx1276::kRegInvertIq2] == (p.invertIq ? sx1276::kInvertIq2Inverted : sx1276::kInvertIq2Normal);
	}
	void start(Sx1276& from, const Sx1276::Sent& p) {
		OnAir a{&from, p, {}};
		for (Sx1276* r : radios) {
			if (r == &from || !matches(from, p, *r)) continue;
			if (r->incoming.on) { r->incoming.collided = true; continue; }
			r->incoming.on = true;
			r->incoming.gen = r->gen;
			r->incoming.collided = false;
			a.listeners.push_back(r);
		}
		flying.push_back(a);
	}
	void tick(uint32_t now) {
		for (Sx1276* r : radios) r->now = now;   // a delivery's DIO0 is stamped with this ms
		for (size_t i = 0; i < flying.size();) {
			if ((int32_t)(now - flying[i].p.end) < 0) { ++i; continue; }
			if (sniff) sniff(*flying[i].from, flying[i].p);
			for (Sx1276* r : flying[i].listeners) {
				if (r->incoming.on && r->incoming.gen == r->gen && !r->incoming.collided && r->inRx()) r->deliver(flying[i].p);
				r->incoming.on = false;
			}
			flying.erase(flying.begin() + (long)i);
		}
	}
};

inline void Sx1276::write(uint8_t r, uint8_t v) {
	using namespace sx1276;
	++writes;
	if (r == kRegFifo) { fifo[reg[kRegFifoAddrPtr]++] = v; return; }
	if (r == kRegIrqFlags) { reg[r] &= (uint8_t)~v; return; }   // write 1 to clear
	if (r == kRegOpMode) {
		const uint8_t old = reg[r];
		if ((old & kOpModeMask) != kOpSleep) v = (uint8_t)((v & ~kOpLoRa) | (old & kOpLoRa));   // LoRa bit: sleep only
		reg[r] = v;
		++gen;
		incoming.on = false;
		if (!lora()) return;
		if (mode() == kOpTx && !txOn) {
			Sent p;
			const uint8_t len = reg[kRegPayloadLength];
			uint8_t a = reg[kRegFifoTxBaseAddr];
			for (uint8_t i = 0; i < len; ++i) p.data.push_back(fifo[a++]);
			p.freqHz = freqHz(); p.sf = sf(); p.bw = bw(); p.crc = crcOn(); p.invertIq = txInverted();
			p.paConfig = reg[kRegPaConfig]; p.paDac = reg[kRegPaDac];
			lora::Config c = lora::defaultConfig(p.freqHz);
			c.sf = p.sf; c.bw = (lora::Bw)p.bw; c.crc = p.crc;
			c.cr = (uint8_t)((reg[kRegModemConfig1] >> 1) & 7);
			c.preamble = (uint16_t)((reg[kRegPreambleMsb] << 8) | reg[kRegPreambleMsb + 1]);
			p.start = now;
			p.end = now + (lora::timeOnAirUs(c, len) + 999) / 1000;
			txOn = true;
			txEnd = p.end;
			sent.push_back(p);
			if (air) air->start(*this, p);
		} else if (mode() == kOpRxSingle || mode() == kOpRxContinuous) {
			rxSince = now;
			rxWriteAddr = reg[kRegFifoRxBaseAddr];
			if (air) air->catchUp(*this);
		}
		return;
	}
	if (r == kRegVersion) return;   // read only
	reg[r & 0x7F] = v;
	if (r == kRegFrfMsb || r == kRegFrfMsb + 1 || r == kRegFrfMsb + 2 || r == kRegModemConfig1 ||
	    r == kRegModemConfig2 || r == kRegModemConfig3 || r == kRegInvertIq || r == kRegInvertIq2 || r == kRegSyncWord) {
		++gen;   // retuned: whatever was being received is lost
	}
}

inline void Sx1276::deliver(const Sent& p) {
	using namespace sx1276;
	const uint8_t start = mode() == kOpRxSingle ? reg[kRegFifoRxBaseAddr] : rxWriteAddr;
	uint8_t a = start;
	for (uint8_t b : p.data) fifo[a++] = b;
	rxWriteAddr = a;
	reg[kRegFifoRxCurrentAddr] = start;
	reg[kRegRxNbBytes] = (uint8_t)p.data.size();
	reg[kRegPktSnrValue] = snrRaw;
	reg[kRegPktRssiValue] = rssiRaw;
	uint8_t flags = kIrqRxDone | kIrqValidHeader;
	if (p.crc && corruptNext > 0) { --corruptNext; flags |= kIrqCrcError; }
	reg[kRegIrqFlags] |= flags;
	++received;
	if (mode() == kOpRxSingle) reg[kRegOpMode] = (uint8_t)((reg[kRegOpMode] & ~kOpModeMask) | kOpStandby);
	if ((reg[kRegDioMapping1] >> 6) == 0 && dio0) dio0();
}

inline void Sx1276::tick(uint32_t t) {
	using namespace sx1276;
	now = t;
	if (txOn && (int32_t)(t - txEnd) >= 0 && !txBroken) {
		txOn = false;
		reg[kRegIrqFlags] |= kIrqTxDone;
		reg[kRegOpMode] = (uint8_t)((reg[kRegOpMode] & ~kOpModeMask) | kOpStandby);
		if ((reg[kRegDioMapping1] >> 6) == 1 && dio0) dio0();
	}
	if (lora() && mode() == kOpRxSingle && !incoming.on) {
		const uint32_t limitMs = (uint32_t)(((uint64_t)lora::symbolUs(sf(), (lora::Bw)bw()) * symbTimeout() + 999) / 1000);
		if (t - rxSince >= limitMs) {
			++timeouts;
			reg[kRegIrqFlags] |= kIrqRxTimeout;
			reg[kRegOpMode] = (uint8_t)((reg[kRegOpMode] & ~kOpModeMask) | kOpStandby);
			if (((reg[kRegDioMapping1] >> 4) & 3) == 0 && dio1) dio1();
		}
	}
}

// ---- transport 1: a bare ISensorTransport, register numbers as they are ----
class Bus : public ISensorTransport {
public:
	explicit Bus(Sx1276& c) : chip(c) {}
	bool writeReg(uint8_t r, uint8_t v) override { return writeRegs(r, &v, 1); }
	bool writeRegs(uint8_t r, const uint8_t* buf, uint8_t len) override {
		if (busy || chip.refuse || len == 0 || len > kMaxWriteLen) return false;
		failed = !chip.present;
		if (chip.present) for (uint8_t i = 0; i < len; ++i) chip.write(r == 0 ? 0 : (uint8_t)(r + i), buf[i]);
		busy = true; polls = 1;
		return true;
	}
	bool readRegs(uint8_t r, uint8_t* buf, uint8_t len) override {
		if (busy || chip.refuse || len == 0 || len > kMaxWriteLen) return false;
		failed = !chip.present;
		for (uint8_t i = 0; i < len; ++i) buf[i] = chip.present ? chip.read(r == 0 ? 0 : (uint8_t)(r + i)) : 0;
		busy = true; polls = 1;
		return true;
	}
	bool writeBytes(const uint8_t*, uint8_t) override { return false; }
	bool readBytes(uint8_t*, uint8_t) override { return false; }
	bool isBusy() const override {
		if (!busy) return false;
		if (chip.stuck) return true;
		if (polls > 0) { --polls; return true; }
		busy = false;
		return false;
	}
	bool lastOpFailed() const override { return failed; }
	bool checkDevice() override { return chip.present; }
private:
	Sx1276& chip;
	mutable bool busy = false; mutable int polls = 0;
	bool failed = false;
};

// ---- transport 2: itransport's real SPITransport, HAL faked ----
// Decodes the SPI bytes as the SX1276 does: bit 7 of the first byte set
// is a write; the address moves on after each byte except for the FIFO.
static int g_sxSpiBus;
class Spi : public SPITransport {
public:
	explicit Spi(Sx1276& c) : SPITransport(&g_sxSpiBus, nullptr, 0, nullptr), chip(c) {}
	int transfers = 0;
protected:
	bool halTransmit(uint8_t* tx, uint16_t n) override { return xfer(tx, nullptr, n); }
	bool halTransmitReceive(uint8_t* tx, uint8_t* rx, uint16_t n) override { return xfer(tx, rx, n); }
	void halCsLow() override {}
	void halCsHigh() override {}
private:
	bool xfer(uint8_t* tx, uint8_t* rx, uint16_t n) {
		if (chip.refuse) return false;
		++transfers;
		if (rx) std::memset(rx, 0, n);
		if (chip.present && n > 0) {
			const bool write = tx[0] & 0x80;
			uint8_t r = tx[0] & 0x7F;
			for (uint16_t i = 1; i < n; ++i) {
				if (write) chip.write(r, tx[i]);
				else if (rx) rx[i] = chip.read(r);
				if (r != 0) ++r;
			}
		}
		BusTransport::onTransferComplete(&g_sxSpiBus, false);
		return true;
	}
	Sx1276& chip;
};

} // namespace sim
