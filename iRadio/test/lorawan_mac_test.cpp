// Host test for lorawan::Mac: a device (Mac over rfm95 over a simulated
// SX1276) and a simulated TTN gateway and network server (sim/
// SimLoRaWanServer.h) on one simulated air, US915 sub-band 2.
//
// g++ -std=c++17 -fno-exceptions -fno-rtti -Wall -Wextra -I../lorawan/inc -I../rfm95/inc -Isim -Istub -I../../isensor/inc -I../../iTransport/itransport/inc lorawan_mac_test.cpp ../lorawan/src/LoRaWanFrame.cpp ../lorawan/src/RegionUS915.cpp ../lorawan/src/LoRaWanMac.cpp ../../iTransport/itransport/src/BusTransport.cpp ../../iTransport/itransport/src/SPITransport.cpp ../../iTransport/itransport/src/DebugLog.cpp -o lorawan_mac_test
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <memory>
#include <string>
#include <vector>
#include "SimSX1276.h"
#include "SimLoRaWanServer.h"
#include "rfm95.h"
#include "RegionUS915.h"
#include "LoRaWanMac.h"
#include "xLoRaWanMac.h"   // with test/stub/cmsis_os2.h

static int g_checks = 0, g_failures = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { ++g_failures; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

using namespace lorawan;

static const uint8_t kDevEui[8] = {0x70, 0xB3, 0xD5, 0x7E, 0xD0, 0x05, 0x12, 0x34};
static const uint8_t kJoinEui[8] = {0, 0, 0, 0, 0, 0, 0, 0};
static const uint8_t kAppKey[16] = {0x2B, 0x7E, 0x15, 0x16, 0x28, 0xAE, 0xD2, 0xA6, 0xAB, 0xF7, 0x15, 0x88, 0x09, 0xCF, 0x4F, 0x3C};

// The session store: RAM, with a failure switch, and a note of how many
// packets the radio had sent at each save.
struct MemStore : iSessionStore {
	std::vector<uint8_t> data;
	bool failSave = false;
	int saves = 0, loads = 0;
	const std::vector<sim::Sx1276::Sent>* sent = nullptr;
	std::vector<size_t> sentAtSave;
	bool load(uint8_t* p, uint16_t len) override {
		++loads;
		if (data.size() != len) return false;
		memcpy(p, data.data(), len);
		return true;
	}
	bool save(const uint8_t* p, uint16_t len) override {
		if (failSave) return false;
		++saves;
		data.assign(p, p + len);
		if (sent) sentAtSave.push_back(sent->size());
		return true;
	}
	uint16_t devNonce() const { return data.size() > 13 ? (uint16_t)(data[12] | data[13] << 8) : 0; }
};

static MacParam param(uint8_t dr = 0) {
	MacParam p = defaultMacParam();
	memcpy(p.devEui, kDevEui, 8);
	memcpy(p.joinEui, kJoinEui, 8);
	memcpy(p.appKey, kAppKey, 16);
	p.dataRate = dr;
	return p;
}

static rfm95_param_t radioParam() {
	rfm95_param_t p = rfm95_default_param();
	p.dio0Interrupt = true;   // TxDone stamped the moment it happens
	p.dio1Interrupt = true;
	return p;
}

// A device: radio, region and MAC, on the air.
struct Dev {
	sim::Sx1276 chip;
	std::unique_ptr<rfm95<sim::Bus>> radio;
	RegionUS915 region{2};
	MemStore* store;
	std::unique_ptr<Mac> mac;
	std::vector<MacEvent> events;
	std::vector<std::vector<uint8_t>> downs;   // each downlink's payload, in order
	Dev(sim::Air& air, MemStore& s, const MacParam& p) : store(&s) {
		air.add(chip);
		radio.reset(new rfm95<sim::Bus>(radioParam(), chip));
		chip.dio0 = [this]() { radio->onDio0(chip.now); };
		chip.dio1 = [this]() { radio->onDio1(chip.now); };
		s.sent = &chip.sent;
		mac.reset(new Mac(*radio, region, s, p));
	}
	void step(uint32_t now) {
		chip.tick(now);
		for (int i = 0; i < 20; ++i) mac->main(now);
		MacEvent e;
		uint8_t buf[242];
		while (mac->takeEvent(&e, buf, sizeof buf)) {
			events.push_back(e);
			if (e.kind == mac_ev_downlink) downs.push_back(std::vector<uint8_t>(buf, buf + e.len));
		}
	}
	int count(MacEventKind k) const { int n = 0; for (auto& e : events) n += e.kind == k; return n; }
	// The last event of a kind; a zeroed one if there was none (so a check
	// on its fields fails instead of crashing).
	const MacEvent* last(MacEventKind k) const {
		static const MacEvent none = MacEvent();
		for (auto it = events.rbegin(); it != events.rend(); ++it) if (it->kind == k) return &*it;
		return &none;
	}
};

struct World {
	sim::Air air;
	sim::LoRaWanServer ns;
	uint32_t t;
	std::vector<Dev*> devs;
	explicit World(uint32_t start = 1000) : t(start) {
		memcpy(ns.devEui, kDevEui, 8);
		memcpy(ns.joinEui, kJoinEui, 8);
		memcpy(ns.appKey, kAppKey, 16);
		ns.attach(air);
	}
	void run(uint32_t ms) {
		for (uint32_t i = 0; i < ms; ++i, ++t) {
			air.tick(t);
			ns.tick(t);
			for (Dev* d : devs) d->step(t);
		}
	}
	template <typename F> bool until(F done, uint32_t maxMs) {
		for (uint32_t i = 0; i < maxMs; ++i) { if (done()) return true; run(1); }
		return done();
	}
};

static bool startJoined(World& w, Dev& d) {
	d.mac->begin(w.t);
	if (!d.mac->join()) return false;
	return w.until([&] { return d.count(mac_ev_joined) > 0; }, 200000);
}

static bool uplink(World& w, Dev& d, uint8_t port, const std::string& s, bool confirmed = false) {
	const int before = d.count(mac_ev_tx_done);
	if (!d.mac->send(port, (const uint8_t*)s.data(), (uint8_t)s.size(), confirmed)) return false;
	return w.until([&] { return d.count(mac_ev_tx_done) > before; }, 60000);
}

static std::string str(const std::vector<uint8_t>& v) { return std::string(v.begin(), v.end()); }

static const sim::LoRaWanServer::Up& lastUp(const World& w) {
	static const sim::LoRaWanServer::Up none = sim::LoRaWanServer::Up();
	return w.ns.ups.empty() ? none : w.ns.ups.back();
}

// ---- join, then uplinks ----
static void testJoinAndUplink() {
	World w;
	MemStore store;
	Dev d(w.air, store, param());
	w.devs.push_back(&d);
	d.mac->begin(w.t);
	CHECK(!d.mac->joined() && !d.mac->send(1, (const uint8_t*)"x", 1, false));   // not joined
	CHECK(d.mac->join());
	CHECK(!d.mac->join());                                                       // busy
	CHECK(w.until([&] { return d.count(mac_ev_joined) > 0; }, 20000));
	CHECK(d.mac->joined() && d.mac->devAddr() == w.ns.devAddr && w.ns.joinRequests == 1);
	CHECK(w.ns.devNonces.size() == 1 && w.ns.devNonces[0] == 0 && d.mac->devNonce() == 1);
	// DevNonce saved before the request went out.
	CHECK(!store.sentAtSave.empty() && store.sentAtSave[0] == 0 && store.devNonce() >= 1);
	// The request: sub-band 2 (channels 8-15 at DR0, or 65 at DR4).
	const sim::Sx1276::Sent& jr = d.chip.sent[0];
	const int ch = sim::us915::upChannel(jr.freqHz);
	CHECK(ch >= 8 && ch <= 15 && jr.sf == 10 && jr.bw == 7 && jr.data.size() == 23 && !jr.invertIq);
	// The CFList put the plan on sub-band 2.
	CHECK(d.region.enabledCount125() == 8 && d.region.enabled(8) && d.region.enabled(65) && !d.region.enabled(0));

	// Uplinks: decrypted by the server, FCnt counting from 0, ADR set.
	CHECK(uplink(w, d, 5, "hello"));
	CHECK(w.ns.ups.size() == 1 && w.ns.ups[0].port == 5 && str(w.ns.ups[0].payload) == "hello" && w.ns.ups[0].fCnt == 0);
	if (w.ns.ups.empty()) return;
	CHECK((w.ns.ups[0].fCtrl & kFCtrlAdr) && !(w.ns.ups[0].fCtrl & kFCtrlAdrAckReq) && w.ns.ups[0].dr == 0);
	CHECK(w.ns.ups[0].ch >= 8 && w.ns.ups[0].ch <= 15);
	CHECK(d.last(mac_ev_tx_done) && !d.last(mac_ev_tx_done)->ack);
	// EIRP 20 dBm (fewer than 50 channels) - 2 dBi: 18 dBm, PA_BOOST with the high power DAC.
	CHECK(w.ns.ups[0].paConfig == (0x80 | (18 - 5)) && w.ns.ups[0].paDac == sx1276::kPaDac20dBm);
	CHECK(uplink(w, d, 6, ""));                       // an empty one, port 6
	CHECK(w.ns.ups.size() == 2 && w.ns.ups[1].fCnt == 1 && w.ns.ups[1].port == 6 && w.ns.ups[1].payload.empty());
	CHECK(w.ns.badMic == 0);
	// Requests the API refuses.
	CHECK(!d.mac->send(0, (const uint8_t*)"x", 1, false) && !d.mac->send(224, (const uint8_t*)"x", 1, false));
	const std::string twelve(12, 'z');
	CHECK(d.mac->maxPayloadNow() == 11 && !d.mac->send(1, (const uint8_t*)twelve.data(), 12, false));
	CHECK(d.mac->send(1, (const uint8_t*)"ab", 2, false) && !d.mac->ready() && !d.mac->send(1, (const uint8_t*)"c", 1, false));
	CHECK(w.until([&] { return d.mac->ready(); }, 10000));
	// Every channel of the round used before one repeats: the join used one,
	// so the first 7 uplinks take the other 7, the next 8 all of them.
	for (int i = 0; i < 12; ++i) CHECK(uplink(w, d, 1, "r"));
	uint32_t first = 0, second = 0;
	if (w.ns.ups.size() < 15) return;
	for (size_t i = 0; i < 7; ++i) first |= 1u << (w.ns.ups[i].ch - 8);
	for (size_t i = 7; i < 15; ++i) second |= 1u << (w.ns.ups[i].ch - 8);
	CHECK(__builtin_popcount(first) == 7 && second == 0xFF);
}

// ---- joins: retries with the backoff, RX2, giving up, an old JoinNonce ----
static void testJoinRetries() {
	World w;
	MemStore store;
	Dev d(w.air, store, param());
	w.devs.push_back(&d);
	w.ns.dropJoins = 2;
	w.ns.joinInRx2 = true;
	d.mac->begin(w.t);
	CHECK(d.mac->join());
	CHECK(w.until([&] { return d.count(mac_ev_joined) > 0; }, 600000));
	CHECK(w.ns.devNonces.size() == 3 && w.ns.devNonces[0] == 0 && w.ns.devNonces[1] == 1 && w.ns.devNonces[2] == 2);
	// 1% duty cycle in the first hour: at least 99 x the time on air between requests.
	const uint32_t toa = (lora::timeOnAirUs(lora::Config{903900000u, 10, lora::Bw::Bw125k, 1, 8, true, false, 14}, 23) + 999) / 1000;
	CHECK(d.chip.sent.size() == 3);
	for (int i = 1; i < 3; ++i) {
		const uint32_t gap = d.chip.sent[i].start - d.chip.sent[i - 1].end;
		CHECK(gap >= toa * 99 && gap <= toa * 99 + 1000 + 50);
	}
	CHECK(d.mac->joined());   // through RX2 (923.3 MHz, DR8)

	// Giving up after maxTries.
	World w2;
	MemStore s2;
	Dev d2(w2.air, s2, param());
	w2.devs.push_back(&d2);
	w2.ns.answerJoins = false;
	d2.mac->begin(w2.t);
	CHECK(d2.mac->join(2));
	CHECK(w2.until([&] { return d2.count(mac_ev_join_failed) > 0; }, 600000));
	CHECK(w2.ns.joinRequests == 2 && !d2.mac->joined() && d2.mac->ready());

	// The same JoinNonce again (a replayed join accept) is refused.
	World w3;
	MemStore s3;
	Dev d3(w3.air, s3, param());
	w3.devs.push_back(&d3);
	CHECK(startJoined(w3, d3));
	const uint32_t addr = d3.mac->devAddr();
	w3.ns.replayJoinNonce = true;
	CHECK(d3.mac->join(1));
	CHECK(w3.until([&] { return d3.count(mac_ev_join_failed) > 0; }, 100000));
	CHECK(!d3.mac->joined() && d3.mac->devAddr() != w3.ns.devAddr && addr != w3.ns.devAddr);
}

// ---- the session across a reset ----
static void testSessionPersistence() {
	World w;
	MemStore store;
	MacParam p = param();
	p.saveEvery = 4;
	{
		Dev d(w.air, store, p);
		w.devs.push_back(&d);
		CHECK(startJoined(w, d));
		for (int i = 0; i < 6; ++i) CHECK(uplink(w, d, 1, "a"));
		CHECK(lastUp(w).fCnt == 5);
		w.devs.clear();
		w.air.radios.erase(std::remove(w.air.radios.begin(), w.air.radios.end(), &d.chip), w.air.radios.end());
	}
	// "Reset": a new device on the same store.
	Dev d(w.air, store, p);
	w.devs.push_back(&d);
	d.mac->begin(w.t);
	CHECK(d.mac->joined() && d.mac->devAddr() == w.ns.devAddr && d.mac->devNonce() == 1);
	CHECK(d.mac->fCntUp() == 8);                         // the saved ceiling: never below what was sent
	CHECK(uplink(w, d, 1, "after reset"));
	CHECK(lastUp(w).fCnt == 8 && str(lastUp(w).payload) == "after reset" && w.ns.badMic == 0);
	// A new join continues the DevNonce count; TTN would refuse a repeat.
	CHECK(d.mac->join());
	CHECK(w.until([&] { return d.count(mac_ev_joined) > 0; }, 100000));
	CHECK(w.ns.devNonces.back() == 1 && w.ns.rejectedNonce == 0);

	// A record that isn't this device's, or is damaged, is ignored.
	MemStore other = store;
	MacParam q = param();
	q.devEui[7] ^= 1;
	World w2;
	Dev d2(w2.air, other, q);
	d2.mac->begin(0);
	CHECK(!d2.mac->joined() && d2.mac->devNonce() == 0);
	MemStore bad = store;
	bad.data[40] ^= 0x55;
	Dev d3(w2.air, bad, p);
	d3.mac->begin(0);
	CHECK(!d3.mac->joined() && d3.mac->devNonce() == 0);
	// A store that can't save: no join request goes out.
	MemStore broken;
	broken.failSave = true;
	World w3;
	Dev d4(w3.air, broken, p);
	w3.devs.push_back(&d4);
	d4.mac->begin(w3.t);
	CHECK(d4.mac->join());
	w3.run(3000);
	CHECK(d4.chip.sent.empty() && d4.last(mac_ev_fault) && d4.last(mac_ev_fault)->fault == mac_fault_store);
	CHECK(d4.mac->ready() && d4.mac->devNonce() == 0);
}

// ---- confirmed uplinks ----
static void testConfirmed() {
	World w;
	MemStore store;
	MacParam p = param();
	p.confirmedTries = 3;
	Dev d(w.air, store, p);
	w.devs.push_back(&d);
	CHECK(startJoined(w, d));
	CHECK(uplink(w, d, 2, "c1", true));
	CHECK(d.last(mac_ev_tx_done)->ack && w.ns.ups.size() == 1 && w.ns.ups[0].confirmed);
	// No ACK: three transmissions of the same frame, each after RX2 plus 1-3 s.
	w.ns.alwaysAckConfirmed = false;
	CHECK(uplink(w, d, 2, "c2", true));
	CHECK(!d.last(mac_ev_tx_done)->ack && w.ns.ups.size() == 4);
	if (w.ns.ups.size() < 4) return;
	for (int i = 1; i < 4; ++i) CHECK(w.ns.ups[(size_t)i].fCnt == 1 && str(w.ns.ups[(size_t)i].payload) == "c2");
	for (int i = 2; i < 4; ++i) {
		const uint32_t gap = w.ns.ups[(size_t)i].start - w.ns.ups[(size_t)i - 1].end;
		CHECK(gap >= 2000 + 1000 && gap <= 2000 + 100 + 3000 + 50);
	}
	// An ACK on the second try stops it there.
	w.ns.alwaysAckConfirmed = true;
	sim::LoRaWanServer::Plan none;
	none.ack = false;
	w.ns.plans.push_back(none);
	CHECK(uplink(w, d, 2, "c3", true));
	CHECK(d.last(mac_ev_tx_done)->ack && w.ns.ups.size() == 6 && w.ns.ups[5].fCnt == 2);
}

// ---- downlinks ----
static void testDownlinks() {
	World w;
	MemStore store;
	Dev d(w.air, store, param(3));
	w.devs.push_back(&d);
	CHECK(startJoined(w, d));
	sim::LoRaWanServer::Plan pl;
	pl.port = 7;
	pl.payload = {'a', 'b', 'c'};
	pl.fPending = true;
	w.ns.plans.push_back(pl);
	CHECK(uplink(w, d, 1, "u1"));
	const MacEvent* e = d.last(mac_ev_downlink);
	CHECK(e && e->port == 7 && e->len == 3 && e->fPending && str(d.downs.back()) == "abc" && e->rssiDbm == -51 && e->snrDb == 10);
	CHECK(d.last(mac_ev_tx_done)->fPending);
	// In RX2, and a confirmed one: the next uplink carries the ACK.
	pl = sim::LoRaWanServer::Plan();
	pl.port = 9; pl.payload = {1, 2}; pl.rx2 = true; pl.confirmed = true;
	w.ns.plans.push_back(pl);
	CHECK(uplink(w, d, 1, "u2"));
	CHECK(d.last(mac_ev_downlink)->port == 9 && d.downs.back() == std::vector<uint8_t>({1, 2}));
	CHECK(!(lastUp(w).fCtrl & kFCtrlAck));
	CHECK(uplink(w, d, 1, "u3"));
	CHECK(lastUp(w).fCtrl & kFCtrlAck);
	CHECK(uplink(w, d, 1, "u4"));
	CHECK(!(lastUp(w).fCtrl & kFCtrlAck));
	// FCntDown past 16 bits; then a replay, a bad MIC and another DevAddr are ignored.
	const int before = d.count(mac_ev_downlink);
	pl = sim::LoRaWanServer::Plan();
	pl.port = 3; pl.payload = {'j'}; pl.fCntJump = 65530;   // a gap under 2^16, then across the 16-bit wrap
	w.ns.plans.push_back(pl);
	CHECK(uplink(w, d, 1, "u5"));
	pl.fCntJump = 10;
	w.ns.plans.push_back(pl);
	CHECK(uplink(w, d, 1, "u5b"));
	CHECK(d.count(mac_ev_downlink) == before + 2 && d.mac->fCntDown() == w.ns.fCntDown - 1 && d.mac->fCntDown() > 0x10000);
	pl = sim::LoRaWanServer::Plan();
	pl.replayLast = true;
	w.ns.plans.push_back(pl);
	CHECK(uplink(w, d, 1, "u6"));
	pl = sim::LoRaWanServer::Plan();
	pl.port = 3; pl.payload = {'x'}; pl.badMic = true;
	w.ns.plans.push_back(pl);
	CHECK(uplink(w, d, 1, "u7"));
	pl = sim::LoRaWanServer::Plan();
	pl.port = 3; pl.payload = {'y'}; pl.devAddrOverride = 0x26000001;
	w.ns.plans.push_back(pl);
	CHECK(uplink(w, d, 1, "u8"));
	CHECK(d.count(mac_ev_downlink) == before + 2);
	// A downlink not taken yet holds the buffer; the next is dropped and counted.
	pl = sim::LoRaWanServer::Plan();
	pl.port = 4; pl.payload = {'p'};
	w.ns.plans.push_back(pl);
	w.ns.plans.push_back(pl);
	auto runRaw = [&]() {   // the device without anyone taking its events
		for (int i = 0; i < 20000 && !d.mac->ready(); ++i) {
			++w.t; w.air.tick(w.t); w.ns.tick(w.t); d.chip.tick(w.t);
			for (int k = 0; k < 20; ++k) d.mac->main(w.t);
		}
	};
	CHECK(d.mac->send(1, (const uint8_t*)"h", 1, false));
	runRaw();
	CHECK(d.mac->send(1, (const uint8_t*)"h", 1, false));
	runRaw();
	CHECK(d.mac->eventsDropped() == 1);
	MacEvent ev;
	uint8_t buf[8];
	int downs = 0;
	while (d.mac->takeEvent(&ev, buf, sizeof buf)) if (ev.kind == mac_ev_downlink) { ++downs; CHECK(buf[0] == 'p'); }
	CHECK(downs == 1);
}

// ---- MAC commands ----
static std::vector<uint8_t> foptsOf(const World& w) { return lastUp(w).fopts; }

static void testMacCommands() {
	World w;
	MemStore store;
	Dev d(w.air, store, param(0));
	w.devs.push_back(&d);
	CHECK(startJoined(w, d));
	sim::LoRaWanServer::Plan pl;

	// LinkADRReq block (two commands): sub-band 2, DR3, TXPower 2, NbTrans 1.
	pl.fopts = {0x03, 0x32, 0x02, 0x00, 0x71, 0x03, 0x32, 0x00, 0xFF, 0x01};
	w.ns.plans.push_back(pl);
	CHECK(uplink(w, d, 1, "a"));
	CHECK(d.mac->adrState().dr == 3 && d.mac->adrState().txPower == 2);
	CHECK(uplink(w, d, 1, "b"));
	CHECK(foptsOf(w) == std::vector<uint8_t>({0x03, 0x07, 0x03, 0x07}) && lastUp(w).dr == 3);
	CHECK(uplink(w, d, 1, "c"));
	CHECK(foptsOf(w).empty());                           // answers go once

	// DevStatusReq, in FOpts and on port 0 (encrypted with the NwkSKey).
	pl = sim::LoRaWanServer::Plan();
	pl.fopts = {0x06};
	w.ns.plans.push_back(pl);
	CHECK(uplink(w, d, 1, "d"));
	CHECK(uplink(w, d, 1, "e"));
	CHECK(foptsOf(w) == std::vector<uint8_t>({0x06, 255, 10}));   // battery unknown, SNR 10 dB
	pl = sim::LoRaWanServer::Plan();
	pl.fopts = {0x06};
	pl.port = 0;
	w.ns.plans.push_back(pl);
	CHECK(uplink(w, d, 1, "f"));
	CHECK(uplink(w, d, 1, "g"));
	CHECK(foptsOf(w) == std::vector<uint8_t>({0x06, 255, 10}));
	// An unknown command ends the parsing: the DevStatusReq after it is not seen.
	pl = sim::LoRaWanServer::Plan();
	pl.fopts = {0x80, 0x06};
	w.ns.plans.push_back(pl);
	CHECK(uplink(w, d, 1, "h"));
	CHECK(uplink(w, d, 1, "i"));
	CHECK(foptsOf(w).empty());

	// RXParamSetupReq: RX1 offset 1, RX2 at DR10 on 923.9 MHz. The answer is
	// repeated until a downlink comes; then downlinks arrive with the new settings.
	pl = sim::LoRaWanServer::Plan();
	pl.fopts = {0x05, 0x1A, 0xD8, 0xF9, 0x8C};   // 9239000 x 100 Hz
	w.ns.plans.push_back(pl);
	CHECK(uplink(w, d, 1, "j"));
	CHECK(d.mac->rx1DrOffset() == 1 && d.mac->rx2Dr() == 10 && d.mac->rx2Freq() == 923900000u);
	CHECK(uplink(w, d, 1, "k"));
	CHECK(foptsOf(w) == std::vector<uint8_t>({0x05, 0x07}));
	CHECK(uplink(w, d, 1, "l"));
	CHECK(foptsOf(w) == std::vector<uint8_t>({0x05, 0x07}));   // still: no downlink yet
	w.ns.rx1Offset = 1; w.ns.rx2Dr = 10; w.ns.rx2Freq = 923900000u;
	pl = sim::LoRaWanServer::Plan();
	pl.port = 11; pl.payload = {'r', '2'}; pl.rx2 = true;
	w.ns.plans.push_back(pl);
	CHECK(uplink(w, d, 1, "m"));
	CHECK(d.last(mac_ev_downlink) && d.last(mac_ev_downlink)->port == 11);
	pl = sim::LoRaWanServer::Plan();
	pl.port = 12; pl.payload = {'r', '1'};
	w.ns.plans.push_back(pl);
	CHECK(uplink(w, d, 1, "n"));
	CHECK(d.last(mac_ev_downlink)->port == 12);          // RX1 at DR13 - 1 = DR12
	CHECK(foptsOf(w).empty());
	// Refused (frequency off the grid): answered 0x06, nothing changed.
	pl = sim::LoRaWanServer::Plan();
	pl.fopts = {0x05, 0x0B, 0xD7, 0xF9, 0x8C};
	w.ns.plans.push_back(pl);
	CHECK(uplink(w, d, 1, "o"));
	CHECK(uplink(w, d, 1, "p"));
	CHECK(foptsOf(w) == std::vector<uint8_t>({0x05, 0x06}) && d.mac->rx2Dr() == 10);
	w.ns.plans.push_back(sim::LoRaWanServer::Plan());   // an empty downlink clears the sticky answer
	w.ns.plans.back().fopts = {};
	CHECK(uplink(w, d, 1, "q"));
	CHECK(uplink(w, d, 1, "q2"));
	CHECK(foptsOf(w).empty());

	// RXTimingSetupReq: RX1 three seconds after the uplink.
	pl = sim::LoRaWanServer::Plan();
	pl.fopts = {0x08, 0x03};
	w.ns.plans.push_back(pl);
	CHECK(uplink(w, d, 1, "s"));
	CHECK(d.mac->rx1DelayS() == 3);
	w.ns.rx1Delay = 3;
	pl = sim::LoRaWanServer::Plan();
	pl.port = 13; pl.payload = {'t'};
	w.ns.plans.push_back(pl);
	CHECK(uplink(w, d, 1, "t"));
	CHECK(foptsOf(w) == std::vector<uint8_t>({0x08}) && d.last(mac_ev_downlink)->port == 13);

	// LinkCheckReq and DeviceTimeReq, and their answers.
	d.mac->requestLinkCheck();
	d.mac->requestDeviceTime();
	pl = sim::LoRaWanServer::Plan();
	pl.fopts = {0x02, 20, 1, 0x0D, 0x10, 0x32, 0x54, 0x76, 0x80};
	w.ns.plans.push_back(pl);
	CHECK(uplink(w, d, 1, "u"));
	CHECK(foptsOf(w) == std::vector<uint8_t>({0x02, 0x0D}));
	const MacEvent* lc = d.last(mac_ev_link_check);
	CHECK(lc && lc->margin == 20 && lc->gateways == 1);
	const MacEvent* dt = d.last(mac_ev_device_time);
	CHECK(dt && dt->gpsSeconds == 0x76543210u && dt->gpsFraction == 0x80 && dt->ticks == lastUp(w).end);

	// DutyCycleReq 4: 1/16, so 15 x the time on air off after each uplink.
	pl = sim::LoRaWanServer::Plan();
	pl.fopts = {0x04, 0x04};
	w.ns.plans.push_back(pl);
	CHECK(uplink(w, d, 1, "v"));
	CHECK(d.mac->maxDutyCycle() == 4);
	const std::string big(200, 'w');
	CHECK(uplink(w, d, 1, big));
	CHECK(foptsOf(w) == std::vector<uint8_t>({0x04}));
	const sim::LoRaWanServer::Up prev = lastUp(w);
	const uint32_t prevToa = prev.end - prev.start;
	CHECK(uplink(w, d, 1, "x"));
	CHECK(lastUp(w).start - prev.end >= prevToa * 15);

	// NewChannelReq / TxParamSetupReq / DlChannelReq: skipped without an answer,
	// and what follows them still read.
	pl = sim::LoRaWanServer::Plan();
	pl.fopts = {0x07, 1, 2, 3, 4, 5, 0x09, 0, 0x0A, 1, 2, 3, 4, 0x06};
	w.ns.plans.push_back(pl);
	CHECK(uplink(w, d, 1, "y"));
	CHECK(uplink(w, d, 1, "z"));
	CHECK(foptsOf(w) == std::vector<uint8_t>({0x06, 255, 10}));
}

// ---- ADR: ADRACKReq after 64 uplinks without a downlink, then the backoff ----
static void testAdrBackoff() {
	World w;
	MemStore store;
	Dev d(w.air, store, param(3));
	w.devs.push_back(&d);
	CHECK(startJoined(w, d));
	// LinkADRReq: sub-band 2, DR3, TXPower 7 (16 dBm EIRP).
	sim::LoRaWanServer::Plan pl;
	pl.fopts = {0x03, 0x37, 0x02, 0x00, 0x71, 0x03, 0x37, 0x00, 0xFF, 0x01};
	w.ns.plans.push_back(pl);
	CHECK(uplink(w, d, 1, "1"));
	CHECK(d.mac->adrState().txPower == 7 && d.mac->adrAckCounter() == 0);
	const size_t base = w.ns.ups.size();
	uint8_t powerAt[130] = {0};
	for (int i = 1; i <= 129; ++i) { CHECK(uplink(w, d, 1, "n")); powerAt[i] = d.mac->adrState().txPower; }
	if (w.ns.ups.size() < base + 129) return;
	auto up = [&](int n) -> const sim::LoRaWanServer::Up& { return w.ns.ups[base + (size_t)n - 1]; };   // n-th since the last downlink
	CHECK(!(up(63).fCtrl & kFCtrlAdrAckReq) && (up(64).fCtrl & kFCtrlAdrAckReq) && (up(100).fCtrl & kFCtrlAdrAckReq));
	CHECK(powerAt[95] == 7 && powerAt[96] == 0);         // ADR_ACK_LIMIT + ADR_ACK_DELAY: default power
	CHECK(up(95).paConfig == (0x80 | (14 - 2)) && up(96).paConfig == (0x80 | (18 - 5)));   // 14 then 18 dBm
	CHECK(up(127).dr == 3 && up(128).dr == 2 && up(129).dr == 2);   // + 2 x ADR_ACK_DELAY: one data rate lower
	// A downlink resets the counter and the flag.
	w.ns.plans.push_back(sim::LoRaWanServer::Plan());
	CHECK(uplink(w, d, 1, "n"));
	CHECK(uplink(w, d, 1, "n"));
	CHECK(d.mac->adrAckCounter() == 1 && !(lastUp(w).fCtrl & kFCtrlAdrAckReq));
}

// ---- the receive window's edges: +-rxErrorMs around the nominal time ----
static void testWindowTiming() {
	struct Case { int err; bool heard; };
	// DR13 (RX1 after a DR3 uplink): 83 symbols of 0.256 ms opened 11 ms
	// early, and the radio listening ~3 ms later; a packet can also be
	// caught up to 6.25 symbols (1.6 ms) into its preamble.
	const Case cases[] = {{0, true}, {9, true}, {-7, true}, {18, false}, {-16, false}};
	for (const Case& c : cases) {
		World w;
		MemStore store;
		Dev d(w.air, store, param(3));
		w.devs.push_back(&d);
		CHECK(startJoined(w, d));
		w.ns.timingErrorMs = c.err;
		sim::LoRaWanServer::Plan pl;
		pl.port = 2; pl.payload = {'w'};
		w.ns.plans.push_back(pl);
		CHECK(uplink(w, d, 1, "t"));
		const bool heard = d.count(mac_ev_downlink) == 1;
		CHECK(heard == c.heard);
		if (heard != c.heard) std::printf("  timing error %d ms: heard %d\n", c.err, heard);
	}
}

// ---- the radio on a 32768 Hz clock ----
// Ticks continue across the ms counter's rollover, and roll over themselves.
struct SimClock : iClock {
	uint32_t* ms; uint32_t ms0, base;
	SimClock(uint32_t* m, uint32_t b) : ms(m), ms0(*m), base(b) {}
	uint32_t at(uint32_t msValue) const { return base + (uint32_t)((uint64_t)(uint32_t)(msValue - ms0) * 32768 / 1000); }
	uint32_t ticksPerSecond() const override { return 32768; }
	uint32_t now() override { return at(*ms); }
};

static void testClock() {
	World w(0xFFFF0000u);   // the ms counter rolls over during the test
	MemStore store;
	Dev d(w.air, store, param(3));
	SimClock clk(&w.t, 0xFFFE0000u);
	d.radio->setClock(&clk);
	w.devs.push_back(&d);
	CHECK(startJoined(w, d));
	sim::LoRaWanServer::Plan pl;
	pl.port = 2; pl.payload = {'k'};
	w.ns.plans.push_back(pl);
	d.mac->requestDeviceTime();
	pl.fopts = {0x0D, 1, 0, 0, 0, 0};
	w.ns.plans.back().fopts = pl.fopts;
	CHECK(uplink(w, d, 1, "clock"));
	CHECK(d.last(mac_ev_downlink) && d.last(mac_ev_downlink)->port == 2);
	const MacEvent* dt = d.last(mac_ev_device_time);
	CHECK(dt && dt->ticks == clk.at(lastUp(w).end));
	CHECK(clk.now() < 0xFFFE0000u);   // the ticks rolled over too
	CHECK(w.t < 0xFFFF0000u);   // it did roll over
}

// ---- xLoRaWanMac: sleeps as long as sleepHintMs() says, woken by DIO ----
// The thread only runs when its sleep ends or an interrupt sets its flag
// (test/stub/cmsis_os2.h: the wait records its timeout and returns).
static void testRtosLoop() {
	World w;
	MemStore store;
	Dev d(w.air, store, param(3));
	w.devs.push_back(&d);
	CHECK(startJoined(w, d));
	g_rtos = SimRtos();
	int token;
	g_rtos.current = &token;
	xLoRaWanMac loop(*d.mac);
	d.chip.dio0 = [&]() { d.radio->onDio0(d.chip.now); loop.wakeFromIsr(); };
	d.chip.dio1 = [&]() { d.radio->onDio1(d.chip.now); loop.wakeFromIsr(); };
	sim::LoRaWanServer::Plan pl;
	pl.port = 2; pl.payload = {'s'};
	w.ns.plans.push_back(pl);
	CHECK(d.mac->send(1, (const uint8_t*)"z", 1, false));
	uint32_t wakeAt = w.t;
	int steps = 0;
	uint32_t longest = 0;
	for (uint32_t i = 0; i < 10000 && !(d.mac->ready() && steps > 0); ++i, ++w.t) {
		w.air.tick(w.t);
		w.ns.tick(w.t);
		d.chip.tick(w.t);
		const bool flagged = (g_rtos.flags[&token] & xLoRaWanMac::kWakeFlag) != 0;
		if ((int32_t)(w.t - wakeAt) >= 0 || flagged) {
			const int blocksBefore = g_rtos.blocks;
			for (int k = 0; k < 20; ++k) loop.step(w.t);   // the radio's transfers take a few steps
			++steps;
			const uint32_t slept = g_rtos.blocks > blocksBefore ? g_rtos.lastTimeout : 0;
			if (slept > longest) longest = slept;
			wakeAt = w.t + (slept ? slept : 1);
		}
	}
	MacEvent e;
	uint8_t buf[8];
	bool got = false;
	while (d.mac->takeEvent(&e, buf, sizeof buf)) got |= e.kind == mac_ev_downlink && buf[0] == 's';
	CHECK(got);                                  // RX1 opened in time from the sleeps alone
	CHECK(g_rtos.sets > 0 && longest == 100 && steps < 400);
}

int main() {
	setvbuf(stdout, nullptr, _IONBF, 0);
	testJoinAndUplink();
	testJoinRetries();
	testSessionPersistence();
	testConfirmed();
	testDownlinks();
	testMacCommands();
	testAdrBackoff();
	testWindowTiming();
	testClock();
	testRtosLoop();
	std::printf("lorawan_mac_test: %d checks, %d failures\n", g_checks, g_failures);
	return g_failures ? 1 : 0;
}
