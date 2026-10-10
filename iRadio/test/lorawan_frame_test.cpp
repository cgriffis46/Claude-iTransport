// Host test for the LoRaWAN pieces below the MAC: LoRaWanFrame (against
// frames made by the independent lora-packet library), RegionUS915
// (against LoRaMac-node's numbers and rules), Mac::rxWindow (against a
// literal copy of LoRaMac-node's formula), and the test server's AES
// inverse cipher (against FIPS-197).
//
// g++ -std=c++17 -fno-exceptions -fno-rtti -Wall -Wextra -I../lorawan/inc -I../rfm95/inc -Isim -I../../iTransport/itransport/inc lorawan_frame_test.cpp ../lorawan/src/LoRaWanFrame.cpp ../lorawan/src/RegionUS915.cpp ../lorawan/src/LoRaWanMac.cpp ../../iTransport/itransport/src/DebugLog.cpp -o lorawan_frame_test
#include <cstdio>
#include <cstring>
#include <string>
#include <sstream>
#include <vector>
#include "LoRaWanFrame.h"
#include "RegionUS915.h"
#include "LoRaWanMac.h"
#include "SimLoRaWanServer.h"

static int g_checks = 0, g_failures = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { ++g_failures; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

using namespace lorawan;

static std::vector<uint8_t> hx(const std::string& s) {
	std::vector<uint8_t> v;
	if (s == "-") return v;
	for (size_t i = 0; i + 1 < s.size(); i += 2) v.push_back((uint8_t)std::stoul(s.substr(i, 2), nullptr, 16));
	return v;
}
static uint32_t be(const std::vector<uint8_t>& v) { uint32_t x = 0; for (uint8_t b : v) x = (x << 8) | b; return x; }

// ---- frames vs lora-packet ----
// From lora-packet 0.9.2 (npm), random fields, made by a script with
// fromFields() and generateSessionKeys10(); 1000 such frames were checked
// the same way when this was written, all matching. Fields as lora-packet
// prints them (EUIs, DevAddr, nonces most significant byte first):
//   JR appKey joinEui devEui devNonce wire
//   JA appKey joinNonce netId devAddr dlSettings rxDelay cflist|- devNonce wire nwkSKey appSKey
//   UP|DN nwkSKey appSKey devAddr fCnt32 confirmed fctrl fopts|- port payload|- wire
static const char* kVectors[] = {
	"JR 8fc013387b6c8e553ce0bbd52fc41918 9626948912784aa7 7a116c19b5106c2f 7946 00a74a7812899426962f6c10b5196c117a0a1f05724b88",
	"JR 2332e27a6a4e8b3e8949f7b0cc703363 9aaad0bfe2bbdee6 04a3fc2f3fee1464 62111 00e6debbe2bfd0aa9a6414ee3f2ffca3049ff240313148",
	"JA 79f84eed6ef6e82f466e8f2533d56be6 211837 7b06dc 6ea0b02d 97 10 - 5935 203f8f27642bfb616bcd212dcf907a2b0c 6bf81b2732538db8030fe00f90328fdd a0711365f5faf7d62f0f5dec07e470d8",
	"JA 8f9695ab9ea04d3c476cad2b86d8fa92 c6ce1f f73f3b 3ad75cb5 109 6 7a683e15a953ce5aa5c7bf70877e638c 1576 20d7515a689a47a529575f84942588956e369a86a21be88d35d36457c29fc5a84b a125b737a64403bdf34684fe1b6ada40 d04dbf3cdeeec8f691f98adcc9944336",
	"UP 2744674350f26179facdfabf78832c7e 082ab69749a09fc6b3818601f310dab1 a9db955f 328507596 1 192 - 0 9579 805f95dba9c0cca000020e9ed59283",
	"UP 6064e410f45435fd27b03d957d3770d2 bfd7376b8e250a1fb5f0db8d247fa5b3 b56c98ba 1545601342 0 32 0206 98 fe7220882246c71856adff100dcda2009d 40ba986cb5223e010206621a39b6696197471fad5dd22c3497223c87a76eef00",
	"UP 8738dd4a5637ac17148ab023eb39c826 1910020b57e62a2aee3043101512372d a9aaf30e 1746159510 1 96 d5 82 ed80899cc5eedbc6a5975353cd49f15bd471d923801eec9a5710ca3c0303ebc0f841a7 800ef3aaa9619647d5520f84ed47c18d77b461c6192b62d26fe454b7100ea69a85f1dedaf374f094e296092ab798ffa840",
	"DN 2d1f481f56ec7a16341aa51432da8949 0cb9b4fc56e3aeb5de0bd544bd8b93ec f358b81f 1348007075 0 160 - 0 6d7c1096236cf3c79077304339a246788c916c372512d600a832a293aae899ef89f895e47cfcdcf3f0 601fb858f3a0a3f40046a98591ad5c4580cf667d4f9c3d742d5109b4041d66d8edf500f7e0eea6f6ea81681383aa9cf7967cfc0b1173",
	"DN fa2f3e9f6cfbf700a82b6b200863485c b2991c18ea2f1af3c52e86dea484a12e 284ee849 616425277 1 32 ff9767afa6321371 76 6b7d400ce26eacb32bfc470574d75f692d4be857f9fc77937668574d287dbaf748582a02f765f05bc2e17c21a1166727472db905587567 a049e84e28283de7ff9767afa63213714c744720ca40f0a8fa7441681490472800bd8505e0558d1af8807932765fcd8cf43d8ab03b4d6f9493c8776e7975fbe75e265681fe283161bcdbf8fa",
	"DN 827b8f81737ee4c1785dee2b85e38eee 8382e07693a641be84f6cfdaa62e190f 63401d56 2034173283 1 160 aac2c4 129 - a0561d4063a36305aac2c481cce4147e",
};

static bool checkVector(const std::string& line, int n) {
	std::istringstream ss(line);
	std::string t;
	ss >> t;
	if (t == "JR") {
		std::string k, j, d, w; unsigned dn;
		ss >> k >> j >> d >> dn >> w;
		uint8_t out[kJoinRequestLen];
		buildJoinRequest(hx(k).data(), hx(j).data(), hx(d).data(), (uint16_t)dn, out);
		return std::vector<uint8_t>(out, out + kJoinRequestLen) == hx(w);
	}
	if (t == "JA") {
		std::string k, jn, net, da, cf, w, nk, ak; unsigned dl, rxd, dn;
		ss >> k >> jn >> net >> da >> dl >> rxd >> cf >> dn >> w >> nk >> ak;
		std::vector<uint8_t> wire = hx(w);
		JoinAccept ja;
		bool ok = parseJoinAccept(hx(k).data(), wire.data(), (uint8_t)wire.size(), &ja);
		ok = ok && ja.joinNonce == be(hx(jn)) && ja.netId == be(hx(net)) && ja.devAddr == be(hx(da));
		ok = ok && ja.dlSettings == dl && ja.rxDelay == rxd && ja.hasCfList == (cf != "-");
		ok = ok && (cf == "-" || memcmp(ja.cfList, hx(cf).data(), 16) == 0);
		uint8_t nwk[16], app[16];
		deriveSessionKeys(hx(k).data(), ja.joinNonce, ja.netId, (uint16_t)dn, nwk, app);
		ok = ok && std::vector<uint8_t>(nwk, nwk + 16) == hx(nk) && std::vector<uint8_t>(app, app + 16) == hx(ak);
		std::vector<uint8_t> bad = hx(w);
		bad[1 + (size_t)n % (bad.size() - 1)] ^= 0x10;
		return ok && !parseJoinAccept(hx(k).data(), bad.data(), (uint8_t)bad.size(), &ja);
	}
	std::string nk, ak, da, fo, ps, pl, w; unsigned long fc; unsigned conf, fb;
	ss >> nk >> ak >> da >> fc >> conf >> fb >> fo >> ps >> pl >> w;
	std::vector<uint8_t> fopts = hx(fo), payload = hx(pl), wire = hx(w);
	const uint32_t addr = be(hx(da));
	if (t == "UP") {
		Uplink u;
		memset(&u, 0, sizeof u);
		u.confirmed = conf != 0; u.devAddr = addr; u.fCtrl = (uint8_t)fb; u.fCnt = (uint32_t)fc;
		u.fOpts = fopts.data(); u.fOptsLen = (uint8_t)fopts.size();
		u.hasPort = true; u.port = (uint8_t)std::stoul(ps);
		u.payload = payload.data(); u.payloadLen = (uint8_t)payload.size();
		uint8_t out[255];
		const uint8_t len = buildUplink(hx(nk).data(), hx(ak).data(), u, out, sizeof out);
		return std::vector<uint8_t>(out, out + len) == wire;
	}
	Downlink d;
	bool ok = parseDownlinkHeader(wire.data(), (uint8_t)wire.size(), &d);
	ok = ok && d.devAddr == addr && d.fCnt16 == (uint16_t)fc && (d.fCtrl & 0xF0) == fb;
	ok = ok && d.mtype == (conf ? mtype_confirmed_down : mtype_unconfirmed_down);
	ok = ok && d.fOptsLen == fopts.size() && (fopts.empty() || memcmp(wire.data() + d.fOptsOff, fopts.data(), fopts.size()) == 0);
	ok = ok && d.hasPort && d.port == std::stoul(ps) && d.payloadLen == payload.size();
	std::vector<uint8_t> w2 = wire;
	ok = ok && !openDownlink(hx(nk).data(), hx(ak).data(), d, (uint32_t)fc + 0x10000u, w2.data(), (uint8_t)w2.size());
	ok = ok && openDownlink(hx(nk).data(), hx(ak).data(), d, (uint32_t)fc, wire.data(), (uint8_t)wire.size());
	return ok && (payload.empty() || memcmp(wire.data() + d.payloadOff, payload.data(), payload.size()) == 0);
}

static void testFrames() {
	int n = 0;
	for (const char* v : kVectors) {
		const bool ok = checkVector(v, n++);
		CHECK(ok);
		if (!ok) std::printf("  vector: %.40s\n", v);
	}
	// lora-packet's README example: "test" on port 1, FCnt 2.
	const std::vector<uint8_t> nwk = hx("44024241ed4ce9a68c6a8bc055233fd3"), app = hx("ec925802ae430ca77fd3dd73cb2cc588");
	Uplink u;
	memset(&u, 0, sizeof u);
	u.devAddr = 0x49BE7DF1; u.fCnt = 2; u.hasPort = true; u.port = 1;
	u.payload = (const uint8_t*)"test"; u.payloadLen = 4;
	uint8_t out[64];
	const uint8_t len = buildUplink(nwk.data(), app.data(), u, out, sizeof out);
	CHECK(std::vector<uint8_t>(out, out + len) == hx("40f17dbe4900020001954378762b11ff0d"));
	// Inconsistent uplinks are refused.
	u.fOptsLen = 1; u.port = 0; const uint8_t cmd = 0x02; u.fOpts = &cmd;
	CHECK(buildUplink(nwk.data(), app.data(), u, out, sizeof out) == 0);   // MAC commands in both places
	u.port = 1; u.fOptsLen = 16;
	CHECK(buildUplink(nwk.data(), app.data(), u, out, sizeof out) == 0);   // FOpts over 15
	u.fOptsLen = 0;
	CHECK(buildUplink(nwk.data(), app.data(), u, out, 16) == 0);           // doesn't fit
	// Malformed downlinks.
	Downlink d;
	const std::vector<uint8_t> shortOne = hx("6001020304000000aabbcc");
	CHECK(!parseDownlinkHeader(shortOne.data(), (uint8_t)shortOne.size(), &d));
	const std::vector<uint8_t> up = hx("40f17dbe4900020001954378762b11ff0d");
	CHECK(!parseDownlinkHeader(up.data(), (uint8_t)up.size(), &d));        // an uplink
	const std::vector<uint8_t> optsTooLong = hx("60010203040f0000aabbccdd");   // FOptsLen 15, 0 there
	CHECK(!parseDownlinkHeader(optsTooLong.data(), (uint8_t)optsTooLong.size(), &d));
	const std::vector<uint8_t> both = hx("6001020304010000020000aabbccdd");   // FOpts and port 0
	CHECK(!parseDownlinkHeader(both.data(), (uint8_t)both.size(), &d));
	const std::vector<uint8_t> major1 = hx("6101020304000000aabbccdd");
	CHECK(!parseDownlinkHeader(major1.data(), (uint8_t)major1.size(), &d));
	std::vector<uint8_t> ja = hx("203f8f27642bfb616bcd212dcf907a2b0c");
	JoinAccept a;
	CHECK(!parseJoinAccept(hx("79f84eed6ef6e82f466e8f2533d56be6").data(), ja.data(), 16, &a));   // length
}

// ---- the test server's AES inverse cipher (FIPS-197 Appendix C.1) ----
static void testAesDecrypt() {
	const std::vector<uint8_t> key = hx("000102030405060708090a0b0c0d0e0f");
	const std::vector<uint8_t> ct = hx("69c4e0d86a7b0430d8cdb78070b4c55a");
	sim::AesDecrypt aes(key.data());
	uint8_t pt[16];
	aes.decrypt(ct.data(), pt);
	CHECK(std::vector<uint8_t>(pt, pt + 16) == hx("00112233445566778899aabbccddeeff"));
	CHECK(aes.sbox[0x00] == 0x63 && aes.sbox[0x53] == 0xED && aes.inv[0x63] == 0x00);
}

// ---- US915 ----
static void testUs915() {
	RegionUS915 all(0), tnn(2);
	CHECK(RegionUS915::channelFrequency(0) == 902300000u && RegionUS915::channelFrequency(8) == 903900000u);
	CHECK(RegionUS915::channelFrequency(63) == 914900000u && RegionUS915::channelFrequency(64) == 903000000u);
	CHECK(RegionUS915::channelFrequency(65) == 904600000u && RegionUS915::channelFrequency(71) == 914200000u);
	CHECK(tnn.rx1Frequency(8, 0) == 923300000u && tnn.rx1Frequency(13, 0) == 926300000u && tnn.rx1Frequency(65, 0) == 923900000u);
	CHECK(tnn.rx1DataRate(0, 0) == 10 && tnn.rx1DataRate(3, 0) == 13 && tnn.rx1DataRate(4, 0) == 13);
	// RX1 data rate by uplink DR (rows) and RX1DROffset (columns), RP002 US915.
	const uint8_t rx1[5][4] = {{10, 9, 8, 8}, {11, 10, 9, 8}, {12, 11, 10, 9}, {13, 12, 11, 10}, {13, 13, 12, 11}};
	bool tableOk = true;
	for (uint8_t up = 0; up < 5; ++up)
		for (uint8_t off = 0; off < 4; ++off) tableOk &= tnn.rx1DataRate(up, off) == rx1[up][off];
	CHECK(tableOk);
	CHECK(tnn.maxPayload(0) == 11 && tnn.maxPayload(1) == 53 && tnn.maxPayload(3) == 242 && tnn.maxPayload(8) == 53);
	lora::Config c;
	CHECK(tnn.txDataRate(0, &c) && c.sf == 10 && c.bw == lora::Bw::Bw125k);
	CHECK(tnn.txDataRate(4, &c) && c.sf == 8 && c.bw == lora::Bw::Bw500k);
	CHECK(!tnn.txDataRate(5, &c) && !tnn.rxDataRate(7, &c) && !tnn.rxDataRate(14, &c));
	CHECK(tnn.rxDataRate(8, &c) && c.sf == 12 && c.bw == lora::Bw::Bw500k);
	CHECK(tnn.rxDataRate(13, &c) && c.sf == 7);
	// Power: 30 dBm - 2/step; 26 at DR4; 20 with fewer than 50 channels.
	CHECK(all.eirpDbm(0, 0) == 30 && all.eirpDbm(0, 4) == 26 && all.eirpDbm(14, 0) == 2);
	CHECK(tnn.eirpDbm(0, 0) == 20 && tnn.eirpDbm(7, 0) == 16);

	// Sub-band 2: every channel 8-15 once a round, nothing else; DR4 on 65.
	uint32_t seen = 0;
	for (int round = 0; round < 3; ++round) {
		uint32_t mask = 0;
		for (int i = 0; i < 8; ++i) {
			uint8_t ch; uint32_t f;
			CHECK(tnn.nextChannel(2, false, (uint32_t)(i * 2654435761u), &ch, &f));
			CHECK(ch >= 8 && ch <= 15 && f == RegionUS915::channelFrequency(ch));
			mask |= 1u << (ch - 8);
		}
		CHECK(mask == 0xFF);
		seen |= mask;
	}
	uint8_t ch; uint32_t f;
	CHECK(tnn.nextChannel(4, false, 7, &ch, &f) && ch == 65 && f == 904600000u);
	CHECK(!tnn.nextChannel(5, false, 7, &ch, &f));
	// Joins: eight DR0, then DR4; each 125 kHz one from the next group (all 8 groups with subBand 0).
	CHECK(tnn.joinDataRate(1) == 0 && tnn.joinDataRate(8) == 0 && tnn.joinDataRate(9) == 4 && tnn.joinDataRate(18) == 4);
	uint32_t groups = 0;
	for (int i = 0; i < 8; ++i) {
		CHECK(all.nextChannel(0, true, (uint32_t)i * 7919u, &ch, &f));
		groups |= 1u << (ch / 8);
	}
	CHECK(groups == 0xFF);
	CHECK(all.nextChannel(4, true, 1, &ch, &f) && ch == 64);
	CHECK(all.nextChannel(4, true, 1, &ch, &f) && ch == 65);

	// LinkADRReq. A block: ChMaskCntl 7 (all 125 kHz off, 500 kHz mask 0x02),
	// then ChMaskCntl 0 with 0xFF00 (channels 8-15): sub-band 2. DR3, power 2, NbTrans 1.
	RegionUS915 r(0);
	AdrState st = {0, 0, 1};
	uint8_t used = 0;
	const uint8_t blk[] = {0x03, 0x32, 0x02, 0x00, 0x70, 0x03, 0x32, 0x00, 0xFF, 0x01};
	CHECK(r.linkAdrReq(blk, sizeof blk, true, &st, &used) == 0x07 && used == 10);
	CHECK(st.dr == 3 && st.txPower == 2 && st.nbTrans == 1);
	CHECK(r.enabledCount125() == 8 && r.enabled(8) && r.enabled(15) && !r.enabled(7) && !r.enabled(16) && r.enabled(65) && !r.enabled(64));
	// Stops at the first command that isn't LinkADRReq.
	const uint8_t one[] = {0x03, 0x2F, 0xFF, 0x00, 0x60, 0x06};
	st = {3, 2, 1};
	CHECK(r.linkAdrReq(one, sizeof one, true, &st, &used) == 0x07 && used == 5);
	CHECK(st.dr == 2 && st.txPower == 2 && r.enabledCount125() == 64);   // power 0xF: kept; ChMaskCntl 6: all on
	// ChMaskCntl 5: bit 1 = channels 8-15 and 65.
	const uint8_t five[] = {0x03, 0x20, 0x02, 0x00, 0x50};
	CHECK(r.linkAdrReq(five, sizeof five, true, &st, &used) == 0x07);
	CHECK(r.enabledCount125() == 8 && r.enabled(8) && r.enabled(65) && !r.enabled(66));
	// Refused, and nothing changed: one 125 kHz channel at DR0-3 (FCC), a
	// DR no channel carries, a power index out of range.
	const uint8_t lone[] = {0x03, 0x00, 0x01, 0x00, 0x70 | 0x00};   // cntl 7: 125 kHz off, 64 on, DR0
	st = {2, 2, 1};
	uint8_t s = r.linkAdrReq(lone, sizeof lone, true, &st, &used);
	CHECK((s & 0x01) == 0 && (s & 0x02) == 0 && st.dr == 2 && r.enabledCount125() == 8);
	const uint8_t dr4none[] = {0x03, 0x40, 0x00, 0x00, 0x40};        // cntl 4: no 500 kHz, DR4
	s = r.linkAdrReq(dr4none, sizeof dr4none, true, &st, &used);
	CHECK(s == 0x05 && r.enabled(65));
	const uint8_t power15[] = {0x03, 0x2E, 0xFF, 0x00, 0x00 | 0x01};   // power 14 is the lowest allowed
	CHECK(r.linkAdrReq(power15, sizeof power15, true, &st, &used) == 0x07 && st.txPower == 14);
	// With ADR off only the channel mask is taken.
	st = {1, 3, 2};
	const uint8_t off[] = {0x03, 0x35, 0x00, 0x00, 0x60 | 0x01 /*NbTrans 1*/, };
	CHECK(r.linkAdrReq(off, sizeof off, false, &st, &used) == 0x07 && st.dr == 1 && st.txPower == 3 && st.nbTrans == 2);
	// ADR backoff helpers and the CFList.
	RegionUS915 b(2);
	CHECK(b.nextLowerDr(4) == 3 && b.nextLowerDr(0) == 0);
	const uint8_t cf[16] = {0, 0, 0xFF, 0, 0, 0, 0, 0, 0x04, 0, 0, 0, 0, 0, 0, 0x01};   // sub-band 3
	b.applyCfList(cf);
	CHECK(b.enabled(16) && b.enabled(23) && !b.enabled(8) && b.enabled(66) && !b.enabled(65));
	uint8_t state[10];
	b.saveState(state);
	b.enableDefaultChannels();
	CHECK(b.enabled(8) && !b.enabled(16));
	CHECK(b.loadState(state, 10) && b.enabled(16) && !b.enabled(8));
	const uint8_t cfWrongType[16] = {0xFF, 0xFF, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x00};
	b.applyCfList(cfWrongType);
	CHECK(b.enabled(16) && !b.enabled(0));
	// RXParamSetupReq.
	CHECK(b.rxParamSetupStatus(1, 10, 923900000u) == 0x07);
	CHECK(b.rxParamSetupStatus(4, 10, 923900000u) == 0x03);
	CHECK(b.rxParamSetupStatus(1, 7, 923900000u) == 0x05);
	CHECK(b.rxParamSetupStatus(1, 10, 923800000u) == 0x06);
	CHECK(b.rxParamSetupStatus(1, 10, 902300000u) == 0x06);
}

// ---- the receive windows, against LoRaMac-node's formula copied as written ----
#define LMN_MAX(a, b) (((a) > (b)) ? (a) : (b))
#define LMN_DIV_CEIL(X, Y) (((X) + (Y) - 1) / (Y))
static void lmnRxWindow(uint32_t tSymbolInUs, uint8_t minRxSymbols, uint32_t rxErrorInMs, uint32_t wakeUpTimeInMs,
                        uint32_t* windowTimeoutInSymbols, int32_t* windowOffsetInMs) {
	*windowTimeoutInSymbols = LMN_MAX(LMN_DIV_CEIL(((2 * minRxSymbols - 8) * tSymbolInUs + 2 * (rxErrorInMs * 1000)), tSymbolInUs), minRxSymbols);
	*windowOffsetInMs = (int32_t)LMN_DIV_CEIL((int32_t)(4 * tSymbolInUs) -
	                                          (int32_t)LMN_DIV_CEIL((*windowTimeoutInSymbols * tSymbolInUs), 2) -
	                                          (int32_t)(wakeUpTimeInMs * 1000), 1000);
}

static void testRxWindow() {
	int mismatches = 0, cases = 0;
	for (int dr = 8; dr <= 13; ++dr) {
		const uint32_t t = lora::symbolUs((uint8_t)(12 - (dr - 8)), lora::Bw::Bw500k);
		for (uint8_t minSym = 6; minSym <= 8; ++minSym)
			for (uint32_t err = 0; err <= 50; err += 5)
				for (uint32_t wake = 0; wake <= 5; ++wake) {
					uint32_t s1; int32_t o1;
					lmnRxWindow(t, minSym, err, wake, &s1, &o1);
					uint16_t s2; int32_t o2;
					lorawan::Mac::rxWindow(t, minSym, err, wake, &s2, &o2);
					++cases;
					if (s1 != s2 || o1 != o2) {
						if (++mismatches < 5) std::printf("  rxWindow dr%d min%u err%u wake%u: %u/%d vs %u/%d\n", dr, minSym, err, wake, s1, o1, s2, o2);
					}
				}
	}
	CHECK(mismatches == 0 && cases == 6 * 3 * 11 * 6);
	// DR13 (SF7, 500 kHz: 256 us symbols), 6 symbols, 10 ms, 3 ms wake: 83
	// symbols (21.2 ms), opened 11 ms early (-12.6 ms, truncated toward zero).
	uint16_t s; int32_t o;
	lorawan::Mac::rxWindow(256, 6, 10, 3, &s, &o);
	CHECK(s == 83 && o == -11);
}

int main() {
	testFrames();
	testAesDecrypt();
	testUs915();
	testRxWindow();
	std::printf("lorawan_frame_test: %d checks, %d failures\n", g_checks, g_failures);
	return g_failures ? 1 : 0;
}
