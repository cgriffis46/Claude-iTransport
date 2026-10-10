// Host test for meshcore::Node: nodes (Node over rfm95 over a simulated
// SX1276) on one simulated air, with radio ranges, a MeshCore-style flood
// repeater written here (decode, drop duplicates, append its hash to the
// path, send again after a random delay, as MeshCore's Mesh::routeRecvPacket
// and getRetransmitDelay do), and a raw transmitter that puts packets made
// by MeshCore's own code on the air.
//
// g++ -std=c++17 -fno-exceptions -fno-rtti -Wall -Wextra -I../crypto/inc -I../meshcore/inc -I../rfm95/inc -I../third_party/monocypher -Isim -Istub -I../../isensor/inc -I../../iTransport/itransport/inc meshcore_node_test.cpp ../meshcore/src/*.cpp monocypher.o monocypher-ed25519.o ../../iTransport/itransport/src/BusTransport.cpp ../../iTransport/itransport/src/SPITransport.cpp ../../iTransport/itransport/src/DebugLog.cpp -o meshcore_node_test
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <string>
#include <vector>
#include "SimSX1276.h"
#include "rfm95.h"
#include "MeshNode.h"
#include "xMeshNode.h"   // with test/stub/cmsis_os2.h

static int g_checks = 0, g_failures = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { ++g_failures; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

using namespace meshcore;

static std::vector<uint8_t> hx(const std::string& s) {
	std::vector<uint8_t> v;
	for (size_t i = 0; i + 1 < s.size(); i += 2) v.push_back((uint8_t)std::stoul(s.substr(i, 2), nullptr, 16));
	return v;
}

static rfm95_param_t meshRadio() {
	rfm95_param_t p = rfm95_default_param();
	p.syncWord = 0x12;
	p.dio0Interrupt = true;
	return p;
}

// The last packet a chip sent, or an empty one.
static sim::Sx1276::Sent lastSent(const sim::Sx1276& c) {
	return c.sent.empty() ? sim::Sx1276::Sent() : c.sent.back();
}

// A node on the air.
struct Dev {
	sim::Sx1276 chip;
	std::unique_ptr<rfm95<sim::Bus>> radio;
	LocalIdentity id;
	std::unique_ptr<Node> node;
	std::vector<NodeEvent> events;
	std::vector<std::string> texts;      // each group text / data payload, in order
	Dev(sim::Air& air, uint8_t seedByte, NodeParam p = defaultNodeParam()) {
		air.add(chip);
		radio.reset(new rfm95<sim::Bus>(meshRadio(), chip));
		chip.dio0 = [this]() { radio->onDio0(chip.now); };
		uint8_t seed[32];
		for (int i = 0; i < 32; ++i) seed[i] = (uint8_t)(seedByte * 31 + i);
		while (!id.fromSeed(seed)) ++seed[0];
		p.randomSeed = 1000u + seedByte;
		node.reset(new Node(*radio, id, p));
	}
	void step(uint32_t now) {
		chip.tick(now);
		for (int i = 0; i < 20; ++i) node->main(now);
		NodeEvent e;
		uint8_t buf[200];
		while (node->takeEvent(&e, buf, sizeof buf)) {
			events.push_back(e);
			if (e.kind == node_ev_group_text || e.kind == node_ev_group_data) texts.push_back(std::string((char*)buf, e.len));
		}
	}
	int count(NodeEventKind k) const { int n = 0; for (auto& e : events) n += e.kind == k; return n; }
	std::string lastText() const { return texts.empty() ? std::string() : texts.back(); }
	const NodeEvent& last(NodeEventKind k) const {
		static const NodeEvent none = NodeEvent();
		for (auto it = events.rbegin(); it != events.rend(); ++it) if (it->kind == k) return *it;
		return none;
	}
};

// A MeshCore repeater, reduced to flood forwarding.
struct Repeater {
	sim::Sx1276 chip;
	std::unique_ptr<rfm95<sim::Bus>> radio;
	uint8_t hash;
	std::vector<std::vector<uint8_t>> seen;
	struct Out { uint32_t at; std::vector<uint8_t> raw; };
	std::deque<Out> out;
	uint32_t rng = 777;
	int forwarded = 0;
	bool listening = false, sending = false;
	Repeater(sim::Air& air, uint8_t h) : hash(h) {
		air.add(chip);
		radio.reset(new rfm95<sim::Bus>(meshRadio(), chip));
		chip.dio0 = [this]() { radio->onDio0(chip.now); };
	}
	void step(uint32_t now) {
		chip.tick(now);
		for (int i = 0; i < 20; ++i) {
			radio->main(now);
			lora::Event e;
			uint8_t buf[255];
			while (radio->takeEvent(&e, buf, sizeof buf)) {
				if (e.kind == lora::ev_tx_done) { sending = false; listening = false; }
				if (e.kind != lora::ev_rx_done) continue;
				Packet p;
				if (!p.decode(buf, e.len) || !p.isFlood() || p.payloadVersion() != 0) continue;
				uint8_t h[kHashSize];
				p.hash(h);
				std::vector<uint8_t> hv(h, h + kHashSize);
				bool dup = false;
				for (auto& s : seen) dup |= s == hv;
				if (dup) continue;
				seen.push_back(hv);
				if (p.hopCount() >= 63 || p.pathBytes() + p.pathHashSize() > kMaxPath) continue;
				memset(p.path + p.pathBytes(), hash, p.pathHashSize());   // our hash, in the path's hash size
				p.pathLen = (uint8_t)((p.pathLen & 0xC0) | (p.hopCount() + 1));
				std::vector<uint8_t> raw(255);
				raw.resize(p.encode(raw.data(), 255));
				// getRetransmitDelay: 0-4 x (airtime x 52/50 / 2).
				const uint32_t air = lora::timeOnAirUs(defaultNodeParam().radio, (uint8_t)raw.size()) / 1000u;
				rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
				out.push_back({now + (rng % 5) * (air * 52 / 50 / 2), raw});
			}
			if (!sending && !out.empty() && (int32_t)(now - out.front().at) >= 0 && radio->accepting()) {
				if (radio->transmit(defaultNodeParam().radio, out.front().raw.data(), (uint8_t)out.front().raw.size())) {
					out.pop_front();
					sending = true;
					listening = false;
					++forwarded;
				}
			}
			if (!sending && !listening && radio->accepting() && radio->receive(defaultNodeParam().radio, 0)) listening = true;
		}
	}
};

// Puts raw bytes on the air from a chip with no driver (MeshCore-made packets).
struct Injector {
	sim::Sx1276 chip;
	explicit Injector(sim::Air& air) { air.add(chip); chip.reg[sx1276::kRegOpMode] = sx1276::kOpLoRa | sx1276::kOpStandby; }
	void send(const std::vector<uint8_t>& raw, uint8_t sync = 0x12) {
		using namespace sx1276;
		const lora::Config c = defaultNodeParam().radio;
		const uint32_t frf = lora::frf(c.freqHz);
		chip.write(kRegFrfMsb, (uint8_t)(frf >> 16)); chip.write(kRegFrfMsb + 1, (uint8_t)(frf >> 8)); chip.write(kRegFrfMsb + 2, (uint8_t)frf);
		chip.write(kRegModemConfig1, (uint8_t)((uint8_t)c.bw << 4 | c.cr << 1));
		chip.write(kRegModemConfig2, (uint8_t)(c.sf << 4 | kCrcOn));
		chip.write(kRegModemConfig3, (uint8_t)(lora::lowDataRateOptimize(c.sf, c.bw) ? kLdro : 0));
		chip.write(kRegPreambleMsb, 0); chip.write(kRegPreambleMsb + 1, (uint8_t)c.preamble);
		chip.write(kRegSyncWord, sync);
		chip.write(kRegInvertIq, kInvertIqNormal);
		chip.write(kRegInvertIq2, kInvertIq2Normal);
		chip.write(kRegFifoTxBaseAddr, 0);
		chip.write(kRegFifoAddrPtr, 0);
		for (uint8_t b : raw) chip.write(kRegFifo, b);
		chip.write(kRegPayloadLength, (uint8_t)raw.size());
		chip.write(kRegOpMode, kOpLoRa | kOpTx);
	}
};

struct World {
	sim::Air air;
	uint32_t t = 1000;
	std::vector<Dev*> devs;
	std::vector<Repeater*> reps;
	std::vector<Injector*> injs;
	void run(uint32_t ms) {
		for (uint32_t i = 0; i < ms; ++i, ++t) {
			air.tick(t);
			for (Injector* j : injs) j->chip.tick(t);
			for (Dev* d : devs) d->step(t);
			for (Repeater* r : reps) r->step(t);
		}
	}
	template <typename F> bool until(F f, uint32_t maxMs) {
		for (uint32_t i = 0; i < maxMs; ++i) { if (f()) return true; run(1); }
		return f();
	}
	void begin() { for (Dev* d : devs) d->node->begin(t); run(20); }
};

// ---- the radio settings and the basics between two nodes ----
static void testTwoNodes() {
	CHECK(preambleForSf(7) == 32 && preambleForSf(8) == 32 && preambleForSf(9) == 16 && preambleForSf(12) == 16);
	const NodeParam dp = defaultNodeParam();
	CHECK(dp.radio.freqHz == 910525000u && dp.radio.sf == 7 && dp.radio.bw == lora::Bw::Bw62_5k && dp.radio.cr == 1);
	CHECK(dp.radio.preamble == 32 && dp.radio.crc && !dp.radio.invertIq && dp.dutyPercent == 50);

	World w;
	Dev a(w.air, 1), b(w.air, 2);
	w.devs = {&a, &b};
	CHECK(a.node->addChannel(kPublicChannelKey, 16) == 0 && b.node->addChannel(kPublicChannelKey, 16) == 0);
	CHECK(a.node->addChannel(kPublicChannelKey, 15) == -1);
	w.begin();
	CHECK(a.node->sendGroupText(0, 1791700000u, "alice", "hello mesh"));
	CHECK(w.until([&] { return b.count(node_ev_group_text) > 0; }, 3000));
	const NodeEvent& e = b.last(node_ev_group_text);
	CHECK(e.channel == 0 && e.timestamp == 1791700000u && e.hops == 0 && b.lastText() == "alice: hello mesh");
	CHECK(e.rssiDbm == -51 && e.snrDb == 10);
	CHECK(a.count(node_ev_sent) == 1 && a.last(node_ev_sent).type == payload_grp_txt);
	// What went out: MeshCore's radio settings and sync word, CRC on.
	const sim::Sx1276::Sent s = lastSent(a.chip);
	CHECK(s.sf == 7 && s.bw == (uint8_t)lora::Bw::Bw62_5k && s.crc && !s.invertIq && s.preamble == 32);
	CHECK(s.freqHz > 910524900u && s.freqHz < 910525100u && a.chip.reg[sx1276::kRegSyncWord] == 0x12);
	CHECK(s.paConfig == (0x80 | (17 - 2)));
	// Group data and an advert.
	const uint8_t reading[5] = {1, 2, 3, 4, 5};
	CHECK(b.node->sendGroupData(0, 0xFF00, reading, 5));
	CHECK(w.until([&] { return a.count(node_ev_group_data) > 0; }, 3000));
	CHECK(a.last(node_ev_group_data).dataType == 0xFF00 && a.lastText() == std::string("\x01\x02\x03\x04\x05", 5));
	AdvertData ad;
	ad.type = adv_sensor;
	ad.hasLocation = true;
	ad.latE6 = 41881832;
	ad.lonE6 = -87623177;
	strcpy(ad.name, "weather-roof");
	CHECK(a.node->sendAdvert(1791700500u, ad));
	CHECK(w.until([&] { return b.count(node_ev_advert) > 0; }, 3000));
	const NodeEvent& av = b.last(node_ev_advert);
	CHECK(memcmp(av.pubKey, a.id.pubKey(), 32) == 0 && av.timestamp == 1791700500u && av.advertDataValid);
	CHECK(av.advert.type == adv_sensor && std::string(av.advert.name) == "weather-roof" && av.advert.latE6 == 41881832);
	// Zero hop advert (direct, no path), heard the same way.
	CHECK(b.node->sendAdvert(1791700600u, ad, false));
	CHECK(w.until([&] { return a.count(node_ev_advert) > 0; }, 3000));
	// Nothing went both ways twice; no node got its own packets back.
	CHECK(a.count(node_ev_group_text) == 0 && b.count(node_ev_group_data) == 0);
	// A channel b doesn't have: not opened, nothing reported.
	const uint8_t secretKey[16] = {7, 7, 7};
	CHECK(a.node->addChannel(secretKey, 16) == 1);
	CHECK(a.node->sendGroupText(1, 1, "alice", "private"));
	w.run(1500);
	CHECK(b.count(node_ev_group_text) == 1 && b.node->stats().unopened == 1);
	// The wrong sync word (LoRaWAN's) isn't heard at all.
	Injector inj(w.air);
	w.injs = {&inj};
	Packet p;
	Channel pub;
	pub.set(kPublicChannelKey, 16);
	CHECK(buildGroupText(pub, 2, "x", "lorawan sync", p));
	std::vector<uint8_t> raw(255);
	raw.resize(p.encode(raw.data(), 255));
	inj.send(raw, 0x34);
	w.run(1500);
	CHECK(b.count(node_ev_group_text) == 1 && b.node->stats().received == 3);   // a's text, advert, private text
	inj.send(raw, 0x12);
	w.run(1500);
	CHECK(b.count(node_ev_group_text) == 2 && b.lastText() == "x: lorawan sync");
	// The same packet again is a duplicate.
	inj.send(raw, 0x12);
	w.run(1500);
	CHECK(b.count(node_ev_group_text) == 2 && b.node->stats().duplicates == 1);
}

// ---- packets made by MeshCore's own code, over the air ----
static void testMeshCorePackets() {
	World w;
	Dev b(w.air, 2);
	Injector inj(w.air);
	w.devs = {&b};
	w.injs = {&inj};
	CHECK(b.node->addChannel(kPublicChannelKey, 16) == 0);
	w.begin();
	inj.send(hx("150011c94bdc2ebadeef020e6c32e02e09892622315c2e23fc271ad7552914d7754a8fe203"));
	CHECK(w.until([&] { return b.count(node_ev_group_text) > 0; }, 2000));
	CHECK(b.lastText() == "station-7: rain 2.3 mm" && b.last(node_ev_group_text).timestamp == 1791700000u);
	inj.send(hx("1900111c3a0823d046fe0217a63d329b805e6d0c9f"));
	CHECK(w.until([&] { return b.count(node_ev_group_data) > 0; }, 2000));
	CHECK(b.last(node_ev_group_data).dataType == 0xFF01 && b.lastText() == std::string("\x12\x34\x56\x78\x9a\xbc", 6));
	inj.send(hx("11025ac3e9f6d876b8299c19b859e01a1d14886c2a81fab3f4ca541ec0db2c1f000052c7842ccb6abd4c04b05c4adc77e1ea69d1cb3482a2c2c6eccc68bb8728f3c2e87dbebfe83827c92ac2e2bc9027512d4f1ff771a0e76730e76390469d4088c75df82171b30c92807e73024f80c6fa48696c6c746f7020525054"));
	CHECK(w.until([&] { return b.count(node_ev_advert) > 0; }, 2000));
	const NodeEvent& av = b.last(node_ev_advert);
	CHECK(av.hops == 2 && av.advert.type == adv_repeater && std::string(av.advert.name) == "Hilltop RPT");
	CHECK(av.advert.latE6 == 41123456 && av.advert.lonE6 == -87654321);
	// A forged advert (signature changed) and malformed packets are dropped.
	std::vector<uint8_t> forged = hx("11005ac3e9f6d876b8299c19b859e01a1d14886c2a81fab3f4ca541ec0db2c1f000052c7842ccb6abd4c04b05c4adc77e1ea69d1cb3482a2c2c6eccc68bb8728f3c2e87dbebfe83827c92ac2e2bc9027512d4f1ff771a0e76730e76390469d4088c75df82171b30c92807e73024f80c6fa48696c6c746f7020525054");
	forged[60] ^= 0x20;
	inj.send(forged);
	w.run(1500);
	inj.send(hx("1103aabb"));            // path runs past the end
	w.run(500);
	inj.send(hx("5100aabbccdd"));        // payload version 2
	w.run(500);
	inj.send(hx("0900aabbccdd"));        // a direct message's type: not handled yet
	w.run(500);
	CHECK(b.count(node_ev_advert) == 1 && b.node->stats().badAdverts == 1);
	CHECK(b.node->stats().malformed == 2 && b.node->stats().ignored == 1);
}

// ---- through a repeater: A and B out of each other's range ----
static void testRepeater() {
	World w;
	Dev a(w.air, 1), b(w.air, 2);
	Repeater r(w.air, 0xA5), r2(w.air, 0xB6);
	w.devs = {&a, &b};
	w.reps = {&r};
	// A hears R; B hears R; A and B don't hear each other. R2 joins later.
	bool useR2 = false;
	w.air.inRange = [&](const sim::Sx1276& from, const sim::Sx1276& to) {
		const bool ab = (&from == &a.chip && &to == &b.chip) || (&from == &b.chip && &to == &a.chip);
		const bool viaR2 = &from == &r2.chip || &to == &r2.chip;
		return !ab && (useR2 || !viaR2);
	};
	a.node->addChannel(kPublicChannelKey, 16);
	b.node->addChannel(kPublicChannelKey, 16);
	w.begin();
	CHECK(a.node->sendGroupText(0, 77, "alice", "via the hill"));
	CHECK(w.until([&] { return b.count(node_ev_group_text) > 0; }, 5000));
	CHECK(b.lastText() == "alice: via the hill" && b.last(node_ev_group_text).hops == 1 && r.forwarded == 1);
	w.run(3000);
	// A heard R send its packet back: a duplicate, not a message.
	CHECK(a.count(node_ev_group_text) == 0 && a.node->stats().duplicates >= 1);
	// Two repeaters: B hears the packet twice, reports it once.
	useR2 = true;
	w.reps = {&r, &r2};
	const uint32_t dupBefore = b.node->stats().duplicates;
	CHECK(a.node->sendGroupText(0, 78, "alice", "two ways"));
	w.run(6000);
	CHECK(r.forwarded == 2 && r2.forwarded >= 1);
	CHECK(b.count(node_ev_group_text) == 2 && b.node->stats().duplicates > dupBefore);
	// A 2-byte path hash size: the repeaters add 2 bytes each.
	NodeParam p2 = defaultNodeParam();
	p2.pathHashSize = 2;
	World w2;
	Dev c(w2.air, 3, p2), d(w2.air, 4);
	Repeater rr(w2.air, 0x3C);
	w2.devs = {&c, &d};
	w2.reps = {&rr};
	w2.air.inRange = [&](const sim::Sx1276& from, const sim::Sx1276& to) {
		return !((&from == &c.chip && &to == &d.chip) || (&from == &d.chip && &to == &c.chip));
	};
	c.node->addChannel(kPublicChannelKey, 16);
	d.node->addChannel(kPublicChannelKey, 16);
	w2.begin();
	CHECK(c.node->sendGroupText(0, 5, "c", "wide hashes"));
	CHECK(w2.until([&] { return d.count(node_ev_group_text) > 0; }, 5000));
	CHECK(d.last(node_ev_group_text).hops == 1);
	const std::vector<uint8_t> relayed = rr.chip.sent.empty() ? std::vector<uint8_t>(4) : rr.chip.sent.back().data;
	CHECK(relayed.size() > 4 && relayed[1] == (0x40 | 1) && relayed[2] == 0x3C && relayed[3] == 0x3C);
}

// ---- listen before talk ----
static void testListenBeforeTalk() {
	for (int forced = 0; forced < 2; ++forced) {
		World w;
		NodeParam pb = defaultNodeParam();
		if (forced) pb.lbtMaxMs = 100;
		Dev b(w.air, 2, pb), c(w.air, 3);
		w.devs = {&b, &c};
		b.node->addChannel(kPublicChannelKey, 16);
		c.node->addChannel(kPublicChannelKey, 16);
		w.begin();
		const std::string longText(150, 'L');
		CHECK(c.node->sendGroupText(0, 1, "carol", longText.c_str()));
		CHECK(w.until([&] { return !c.chip.sent.empty(); }, 1000));
		const sim::Sx1276::Sent cs = lastSent(c.chip);
		w.run(150);   // its header has been heard by now (~90 ms)
		CHECK(b.radio->channelBusy() && (int32_t)(w.t - cs.end) < 0);
		CHECK(b.node->sendGroupText(0, 2, "bob", "me too"));
		CHECK(w.until([&] { return b.count(node_ev_sent) > 0; }, 8000));
		const sim::Sx1276::Sent bs = lastSent(b.chip);
		if (!forced) {
			CHECK(bs.start >= cs.end && b.node->stats().lbtWaits > 0 && b.node->stats().lbtForced == 0);
			CHECK(b.count(node_ev_group_text) == 1);           // and carol's packet was received whole
			CHECK(w.until([&] { return c.count(node_ev_group_text) > 0; }, 3000));
		} else {
			CHECK(bs.start < cs.end && b.node->stats().lbtForced == 1);   // gave up waiting after 100 ms
			CHECK((int32_t)(bs.start - cs.start) >= 100 && b.count(node_ev_group_text) == 0);
		}
		CHECK(!b.radio->channelBusy() || b.radio->receivingContinuous());
	}
}

// ---- the airtime budget ----
static void testBudget() {
	World w;
	NodeParam p = defaultNodeParam();
	p.dutyPercent = 1;   // 36 s of airtime an hour
	Dev a(w.air, 1, p), b(w.air, 2);
	w.devs = {&a, &b};
	a.node->addChannel(kPublicChannelKey, 16);
	b.node->addChannel(kPublicChannelKey, 16);
	w.begin();
	CHECK(a.node->budgetMs() == 36000);
	const uint32_t longest = lora::timeOnAirUs(p.radio, 255) / 1000u;
	const std::vector<uint8_t> big(165, 0xEE);
	uint32_t used = 0;
	int sent = 0;
	// Send until the budget holds less than half the longest packet.
	while (a.node->budgetMs() >= longest / 2 && sent < 200) {
		const int before = a.count(node_ev_sent);
		CHECK(a.node->sendGroupData(0, 0xFF02, big.data(), (uint8_t)big.size()));
		if (!w.until([&] { return a.count(node_ev_sent) > before; }, 5000)) break;
		used += lastSent(a.chip).end - lastSent(a.chip).start;
		++sent;
	}
	CHECK(sent > 40 && a.node->budgetMs() < longest / 2 && a.node->budgetMs() + used >= 36000 - 2000);
	// The next one waits until enough has been refilled at 1%.
	const uint32_t need = longest / 2 - a.node->budgetMs();
	const uint32_t t0 = w.t;
	const int before = a.count(node_ev_sent);
	CHECK(a.node->sendGroupData(0, 0xFF02, big.data(), 10));
	CHECK(w.until([&] { return a.count(node_ev_sent) > before; }, need * 100 + 60000));
	CHECK(w.t - t0 >= need * 100 - 200 && a.node->stats().budgetWaits > 0);
}

// ---- queue, events, faults ----
static void testLimitsAndFaults() {
	World w;
	Dev a(w.air, 1), b(w.air, 2);
	w.devs = {&a, &b};
	a.node->addChannel(kPublicChannelKey, 16);
	b.node->addChannel(kPublicChannelKey, 16);
	w.begin();
	// Three queued; the fourth is refused.
	CHECK(a.node->sendGroupText(0, 1, "a", "1") && a.node->sendGroupText(0, 2, "a", "2") && a.node->sendGroupText(0, 3, "a", "3"));
	CHECK(!a.node->sendGroupText(0, 4, "a", "4") && a.node->stats().queueFull == 1 && a.node->queued() == 3);
	CHECK(!a.node->sendGroupText(5, 4, "a", "no such channel"));
	CHECK(w.until([&] { return b.count(node_ev_group_text) == 3; }, 10000));
	CHECK(b.texts.size() == 3 && b.texts[0] == "a: 1" && b.texts[1] == "a: 2" && b.texts[2] == "a: 3");
	// Events not taken: the fourth is dropped and counted.
	uint32_t t = w.t;
	int queuedTexts = 0;
	for (uint32_t i = 0; i < 8000; ++i, ++t) {   // b's node runs, nobody takes its events
		if (queuedTexts < 4 && a.node->queued() == 0 && a.node->sendGroupText(0, (uint32_t)(10 + queuedTexts), "a", "x")) ++queuedTexts;
		w.air.tick(t);
		a.step(t);
		b.chip.tick(t);
		for (int k = 0; k < 20; ++k) b.node->main(t);
	}
	w.t = t;
	CHECK(b.node->stats().eventsDropped >= 1);
	// A transmission that never finishes: the radio faults, then comes back
	// and the node listens again.
	World w2;
	Dev c(w2.air, 3), d(w2.air, 4);
	w2.devs = {&c, &d};
	c.node->addChannel(kPublicChannelKey, 16);
	d.node->addChannel(kPublicChannelKey, 16);
	w2.begin();
	c.chip.txBroken = true;
	CHECK(c.node->sendGroupText(0, 1, "c", "lost"));
	CHECK(w2.until([&] { return c.count(node_ev_fault) > 0; }, 5000));
	c.chip.txBroken = false;
	w2.run(2500);   // the radio's restart
	CHECK(d.node->sendGroupText(0, 2, "d", "are you back?"));
	CHECK(w2.until([&] { return c.count(node_ev_group_text) > 0; }, 5000));
	CHECK(c.lastText() == "d: are you back?");
	CHECK(c.node->sendGroupText(0, 3, "c", "yes"));
	CHECK(w2.until([&] { return d.count(node_ev_group_text) > 0; }, 5000));
	// A fault while listening (the SPI bus stops answering): the node
	// listens again once the radio is back.
	w2.run(2000);
	c.chip.stuck = true;
	CHECK(w2.until([&] { return c.count(node_ev_fault) > 1; }, 5000));
	c.chip.stuck = false;
	w2.run(2500);
	CHECK(d.node->sendGroupText(0, 4, "d", "still there?"));
	CHECK(w2.until([&] { return c.count(node_ev_group_text) > 1; }, 5000));
	CHECK(c.lastText() == "d: still there?");
	// The duplicate table forgets the oldest after kSeen packets.
	Packet p;
	Channel ch;
	ch.set(kPublicChannelKey, 16);
	uint8_t h0[kHashSize], h[kHashSize];
	buildGroupText(ch, 1000, "z", "first", p);
	p.hash(h0);
	CHECK(c.node->sendPacket(p) && c.node->seen(h0));
	for (int i = 0; i < Node::kSeen; ++i) {
		buildGroupText(ch, (uint32_t)(2000 + i), "z", "fill", p);
		p.hash(h);
		while (!c.node->sendPacket(p)) w2.run(50);
	}
	CHECK(!c.node->seen(h0) && c.node->seen(h));
}

// ---- xMeshNode: the RTOS loop wakes for DIO0, sleeps otherwise ----
static void testRtosLoop() {
	World w;
	Dev a(w.air, 1), b(w.air, 2);
	w.devs = {&a};   // b runs by hand, through xMeshNode
	a.node->addChannel(kPublicChannelKey, 16);
	b.node->addChannel(kPublicChannelKey, 16);
	a.node->begin(w.t);
	b.node->begin(w.t);
	g_rtos = SimRtos();
	int token;
	g_rtos.current = &token;
	xMeshNode loop(*b.node);
	b.chip.dio0 = [&]() { b.radio->onDio0(b.chip.now); loop.wakeFromIsr(); };
	CHECK(a.node->sendGroupText(0, 9, "a", "wake up"));
	uint32_t wakeAt = w.t;
	int steps = 0;
	uint32_t longest = 0;
	for (uint32_t i = 0; i < 4000 && b.count(node_ev_group_text) == 0; ++i, ++w.t) {
		w.air.tick(w.t);
		a.step(w.t);
		b.chip.tick(w.t);
		const bool flagged = (g_rtos.flags[&token] & xMeshNode::kWakeFlag) != 0;
		if ((int32_t)(w.t - wakeAt) >= 0 || flagged) {
			const int blocks = g_rtos.blocks;
			for (int k = 0; k < 20; ++k) loop.step(w.t);
			++steps;
			const uint32_t slept = g_rtos.blocks > blocks ? g_rtos.lastTimeout : 0;
			if (slept > longest) longest = slept;
			wakeAt = w.t + (slept ? slept : 1);
			NodeEvent e;
			uint8_t buf[200];
			while (b.node->takeEvent(&e, buf, sizeof buf)) { b.events.push_back(e); b.texts.push_back(std::string((char*)buf, e.len)); }
		}
	}
	CHECK(b.count(node_ev_group_text) == 1 && b.lastText() == "a: wake up");
	CHECK(longest == 100 && steps < 200 && g_rtos.sets > 0);
}

int main() {
	setvbuf(stdout, nullptr, _IONBF, 0);
	testTwoNodes();
	testMeshCorePackets();
	testRepeater();
	testListenBeforeTalk();
	testBudget();
	testLimitsAndFaults();
	testRtosLoop();
	std::printf("meshcore_node_test: %d checks, %d failures\n", g_checks, g_failures);
	return g_failures ? 1 : 0;
}
