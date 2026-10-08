/*
 * SimDavis.h
 *
 * Simulations for the iRadio host tests: an RFM69 at register level,
 * Davis ISS transmitters that hop on the real timing, and two transports
 * to reach the radio through (a bare ISensorTransport, and itransport's
 * real SPITransport with the HAL calls faked).
 *
 * A packet is on the air for 6.7 ms (4 preamble, 2 sync and 10 data
 * bytes at 19.2 kb/s), and PayloadReady rises as it ends. The radio
 * receives it only if, from 1 ms before it starts (to settle) until it
 * ends, it stays in RX on exactly that channel's frequency, configured
 * for Davis (the settings checked are the ones that matter on the air),
 * with the signal above its RSSI threshold and the last packet read out
 * of the FIFO. So a driver that tunes late, retunes in the middle, picks
 * the wrong channel or has a wrong setting misses packets.
 */
#pragma once
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <vector>
#include "ISensorTransport.h"
#include "SPITransport.h"
#include "DavisProtocol.h"
#include "RFM69Regs.h"

namespace sim {

using namespace DAVIS;

// ---- RFM69 ----
struct Rfm69 {
	uint8_t reg[0x80];
	std::deque<uint8_t> fifo;
	bool payloadReady = false;
	bool present = true;		// answers at all
	bool stuck = false;			// transfers never complete
	bool refuse = false;		// transport cannot start a transfer
	uint8_t version = RFM69_VERSION;
	uint32_t now = 0;
	std::function<void()> dio0;	// called as PayloadReady rises
	int received = 0, lostFifoFull = 0, flagReads = 0, writes = 0;

	static constexpr uint32_t kSettleMs = 1;
	uint32_t tuneGen = 0;		// counts mode and frequency changes
	uint32_t rxSince = 0;		// when RX was last entered
	struct Incoming {
		bool on = false;
		uint32_t gen = 0;
		uint8_t raw[kPacketLen];
		int rssi = 0;
		int16_t fei = 0;
	} incoming;

	Rfm69() {
		std::memset(reg, 0, sizeof reg);
		reg[RFM69_REG_OPMODE] = RFM69_OPMODE_STANDBY;
		reg[RFM69_REG_SYNCVALUE1] = 0x01;		// reset values
		reg[RFM69_REG_SYNCVALUE2] = 0x01;
		reg[RFM69_REG_RSSITHRESH] = 0xE4;
	}
	uint8_t mode() const { return (uint8_t)(reg[RFM69_REG_OPMODE] & 0x1C); }
	bool inRx() const { return mode() == RFM69_OPMODE_RX; }
	uint32_t frf() const { return ((uint32_t)reg[7] << 16) | ((uint32_t)reg[8] << 8) | reg[9]; }
	bool davisConfigured() const {
		return reg[RFM69_REG_DATAMODUL] == 0x02
			&& reg[RFM69_REG_BITRATEMSB] == 0x06 && reg[RFM69_REG_BITRATELSB] == 0x83
			&& reg[RFM69_REG_FDEVMSB] == 0x00 && reg[RFM69_REG_FDEVLSB] == 0xA1
			&& (reg[RFM69_REG_SYNCCONFIG] & 0xB8) == 0x88		// sync on, 2 bytes
			&& reg[RFM69_REG_SYNCVALUE1] == 0xCB && reg[RFM69_REG_SYNCVALUE2] == 0x89
			&& (reg[RFM69_REG_PACKETCONFIG1] & 0x90) == 0x00		// fixed length, no CRC
			&& reg[RFM69_REG_PAYLOADLENGTH] == kPacketLen
			&& (reg[RFM69_REG_DIOMAPPING1] & 0xC0) == RFM69_DIO0_PAYLOADREADY;
	}

	void write(uint8_t r, uint8_t v) {
		++writes;
		if (r == RFM69_REG_FIFO) return;
		if (r == RFM69_REG_IRQFLAGS2) {
			if (v & RFM69_IRQ2_FIFOOVERRUN) { fifo.clear(); payloadReady = false; }
			return;
		}
		reg[r & 0x7F] = v;
		if (r == RFM69_REG_OPMODE || (r >= RFM69_REG_FRFMSB && r <= RFM69_REG_FRFLSB)) {
			++tuneGen;
			if (r == RFM69_REG_OPMODE && inRx()) rxSince = now;
		}
	}
	uint8_t read(uint8_t r) {
		switch (r) {
		case RFM69_REG_FIFO: {
			if (fifo.empty()) return 0;
			const uint8_t b = fifo.front();
			fifo.pop_front();
			if (fifo.empty()) payloadReady = false;		// and AutoRxRestart re-arms RX
			return b;
		}
		case RFM69_REG_VERSION: return version;
		case RFM69_REG_IRQFLAGS1: return (uint8_t)(RFM69_IRQ1_MODEREADY | (inRx() ? RFM69_IRQ1_RXREADY : 0));
		case RFM69_REG_IRQFLAGS2:
			++flagReads;
			return (uint8_t)((payloadReady ? RFM69_IRQ2_PAYLOADREADY : 0) | (fifo.empty() ? 0 : 0x40));
		default: return reg[r & 0x7F];
		}
	}

	// A packet starts on the air: ten bytes in normal bit order, sent LSB
	// first. The radio locks on to it if it is listening right there.
	void airStart(uint32_t frfAir, const uint8_t raw[kPacketLen], int rssiDbm, int16_t feiSteps) {
		incoming.on = false;
		if (!present || !inRx() || frf() != frfAir || !davisConfigured()) return;
		if ((int32_t)(now - rxSince) < (int32_t)kSettleMs) return;
		if (rssiDbm < -(int)reg[RFM69_REG_RSSITHRESH] / 2) return;
		incoming.on = true;
		incoming.gen = tuneGen;
		std::memcpy(incoming.raw, raw, kPacketLen);
		incoming.rssi = rssiDbm;
		incoming.fei = feiSteps;
	}
	// It ends: PayloadReady, if the radio stayed on it the whole time.
	bool airEnd() {
		if (!incoming.on) return false;
		incoming.on = false;
		if (!present || !inRx() || incoming.gen != tuneGen) return false;
		if (payloadReady) { ++lostFifoFull; return false; }
		fifo.clear();
		for (uint8_t i = 0; i < kPacketLen; ++i) fifo.push_back(reverseBits(incoming.raw[i]));
		reg[RFM69_REG_RSSIVALUE] = (uint8_t)(-incoming.rssi * 2);
		reg[RFM69_REG_FEIMSB] = (uint8_t)((uint16_t)incoming.fei >> 8);
		reg[RFM69_REG_FEILSB] = (uint8_t)((uint16_t)incoming.fei & 0xFF);
		payloadReady = true;
		++received;
		if (dio0) dio0();
		return true;
	}
};

// ---- a Davis station ----
// Sends every (41 + id) / 16 s exactly, one channel on each time, with a
// rotating set of messages.
struct Iss {
	uint8_t id;
	davis_band_t band;
	uint8_t channel;
	uint32_t next16;			// the next send, in sixteenths of a ms
	bool on = true;
	int rssi = -70;
	int16_t feiSteps = -20;		// about -1.2 kHz
	bool corruptNext = false;
	uint32_t sent = 0;
	uint8_t seq = 0;
	uint8_t rainCounter = 120;	// wraps at 128

	float tempF = 72.5f;
	uint16_t humidityTenths = 456;

	Iss(uint8_t id_, davis_band_t b, uint8_t startChannel, uint32_t firstMs)
		: id(id_), band(b), channel(startChannel), next16(firstMs * 16u) {}

	void make(uint8_t out[kPacketLen]) {
		uint8_t d[6] = {0};
		static const uint8_t kTypes[4] = { DAVIS_MSG_TEMP, DAVIS_MSG_HUMIDITY, DAVIS_MSG_RAIN, DAVIS_MSG_GUST };
		const uint8_t type = kTypes[seq++ % 4];
		d[0] = (uint8_t)((type << 4) | id);
		d[1] = 7;					// 7 mph
		d[2] = 128;					// about 180 degrees
		switch (type) {
		case DAVIS_MSG_TEMP: {
			const int16_t raw = (int16_t)(tempF * 160.0f);
			d[3] = (uint8_t)((uint16_t)raw >> 8);
			d[4] = (uint8_t)(raw & 0xFF);
			break;
		}
		case DAVIS_MSG_HUMIDITY:
			d[3] = (uint8_t)(humidityTenths & 0xFF);
			d[4] = (uint8_t)((humidityTenths >> 8) << 4);
			break;
		case DAVIS_MSG_RAIN:
			rainCounter = (uint8_t)((rainCounter + 3) & 0x7F);
			d[3] = rainCounter;
			break;
		case DAVIS_MSG_GUST:
			d[3] = 15;
			d[5] = 0x30;
			break;
		}
		buildPacket(d, out);
		if (corruptNext) { out[4] ^= 0x01; corruptNext = false; }
	}

	// 16 bytes at 19.2 kb/s: 6.67 ms, in sixteenths of a ms.
	static constexpr uint32_t kAir16 = 16u * 8u * 16000u / 19200u;
	bool sending = false;

	// Called every ms. next16 is when a packet ends (when PayloadReady
	// rises); it starts kAir16 before.
	void tick(uint32_t nowMs, Rfm69& radio) {
		const uint32_t now16 = nowMs * 16u;
		for (;;) {
			if (!sending && (int32_t)(now16 - (next16 - kAir16)) >= 0) {
				if (on) {
					uint8_t pkt[kPacketLen];
					make(pkt);
					const uint8_t* f = bandFrf(band, channel);
					const uint32_t frf = ((uint32_t)f[0] << 16) | ((uint32_t)f[1] << 8) | f[2];
					radio.airStart(frf, pkt, rssi, feiSteps);
					++sent;
				}
				sending = true;
			}
			if (sending && (int32_t)(now16 - next16) >= 0) {
				if (on) radio.airEnd();
				sending = false;
				next16 += intervalSixteenths(id);
				channel = (uint8_t)((channel + 1) % bandChannels(band));
				continue;
			}
			break;
		}
	}
};

// ---- transport 1: a bare ISensorTransport, register numbers as they are ----
class Bus : public ISensorTransport {
public:
	explicit Bus(Rfm69& c) : chip(c) {}
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
	Rfm69& chip;
	mutable bool busy = false; mutable int polls = 0;
	bool failed = false;
};

// ---- transport 2: itransport's real SPITransport, HAL faked ----
// Decodes what goes over SPI the way the RFM69 does: bit 7 of the first
// byte set is a write, clear is a read, and the address moves on after
// each byte except for the FIFO.
static int g_spiBus;
class Spi : public SPITransport {
public:
	explicit Spi(Rfm69& c) : SPITransport(&g_spiBus, nullptr, 0, nullptr), chip(c) {}
	int transfers = 0;
protected:
	bool halTransmit(uint8_t* tx, uint16_t n) override {
		if (chip.refuse) return false;
		++transfers;
		exchange(tx, nullptr, n);
		BusTransport::onTransferComplete(&g_spiBus, false);
		return true;
	}
	bool halTransmitReceive(uint8_t* tx, uint8_t* rx, uint16_t n) override {
		if (chip.refuse) return false;
		++transfers;
		exchange(tx, rx, n);
		BusTransport::onTransferComplete(&g_spiBus, false);
		return true;
	}
	void halCsLow() override {}
	void halCsHigh() override {}
private:
	void exchange(const uint8_t* tx, uint8_t* rx, uint16_t n) {
		if (rx) rx[0] = 0;
		if (!chip.present || n == 0) { if (rx) std::memset(rx, 0, n); return; }
		const bool write = tx[0] & 0x80;
		uint8_t r = tx[0] & 0x7F;
		for (uint16_t i = 1; i < n; ++i) {
			if (write) chip.write(r, tx[i]);
			else if (rx) rx[i] = chip.read(r);
			if (r != 0) ++r;
		}
	}
	Rfm69& chip;
};

} // namespace sim
