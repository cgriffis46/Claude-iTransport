// Host test for meshcore::Packet, the channel crypto and the advert and
// group messages, against packets made by MeshCore's own code.
//
// The vectors below came from a small program linked with MeshCore's
// src/Packet.cpp, src/Utils.cpp and src/Identity.cpp, its lib/ed25519
// (orlp) and the rweather Crypto library it uses (MeshCore at
// meshcore-dev/MeshCore a366955, September 2026). createAdvert's and
// createGroupDatagram's few lines were copied into that program as written
// in Mesh.cpp; AdvertDataBuilder and the group plaintexts as in
// AdvertDataHelpers.cpp and BaseChatMesh.cpp. 900 random ones (adverts,
// group packets, packets with paths and transport codes) all matched when
// this was written; these are a sample.
//
// g++ -std=c++17 -fno-exceptions -fno-rtti -Wall -Wextra -I../crypto/inc -I../meshcore/inc -I../third_party/monocypher meshcore_packet_test.cpp ../meshcore/src/MeshPacket.cpp ../meshcore/src/MeshCrypto.cpp ../meshcore/src/MeshIdentity.cpp ../meshcore/src/MeshMessages.cpp monocypher.o monocypher-ed25519.o -o meshcore_packet_test
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>
#include "MeshMessages.h"

static int g_checks = 0, g_failures = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { ++g_failures; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

using namespace meshcore;

static std::vector<uint8_t> hx(const std::string& s) {
	std::vector<uint8_t> v;
	if (s == "-") return v;
	for (size_t i = 0; i + 1 < s.size(); i += 2) v.push_back((uint8_t)std::stoul(s.substr(i, 2), nullptr, 16));
	return v;
}
static std::vector<uint8_t> bytes(const uint8_t* p, size_t n) { return std::vector<uint8_t>(p, p + n); }

//   ADV seed pubKey timestamp appData wire packetHash
//   GRP type key keyHash plaintext wire packetHash
//   PKT wire header pathLen transport0 transport1 path payload packetHash
static const char* kVectors[] = {
	"ADV 40ef83fc7c858f774b8e73516b852235734805e26c053b530c8efb509b2658e0 4ac7417b7b741cd3c04ad4e8456adfca94df193a790cdc21489f08b56611d00b 223155947 4a 11004ac7417b7b741cd3c04ad4e8456adfca94df193a790cdc21489f08b56611d00beb164d0dcc2bee8cb78ce996ee03ca66bb0cd53737aef650e658c042f574b48b662761d320e5fa76421bc07665938cc71e21262e1b1715304ca98d36e916dd13fac106034a b4b8d335398d3ac9",
	"ADV 4c160cf83e2992b252b92e3a179507513f7b00daacb98e9aab9ee32003c006c2 9d970b814eab6c88d2cc4c53da5bb5a6767a29372548c003148ba46c5ae3e706 27045809 7cec25c82b3ed61541e005fbe87279bed4c779452d9274c8cb92853c762589 11009d970b814eab6c88d2cc4c53da5bb5a6767a29372548c003148ba46c5ae3e706b1af9c01678af2ce61085db083bcaa9f6a118322eee4aebc21e1aa13719ed05c2f7864771793b0210ad96803ecf08e8c9b79248e3369911ed7985ee2bbdf456e112b7b0c7cec25c82b3ed61541e005fbe87279bed4c779452d9274c8cb92853c762589 d35496aa3c78038d",
	"GRP 5 a4cabd9db5e707965eb238657253c6b598a3aadbd6fdbcd5395b5fc12801d3ac 0a 813a2848ddc7a2e2f7dbd258da6843116f170d768fc0a64c7b1ccd1c6469286aa3b40a2f53894f005f6d03f9ca6422998e4cc891842a41ee940e8d828226f90f5f0cd34368f18d4fb352352eaaefc178593cc53d235af6a23b700725a6c28f2a332c74516c7c08f03652e4d33b855d00c1916f96c5abf8a92e4d130165cb6854d5bafca12fbf797c6e6f3a1d2c36ebc8bdd40effc9979abe0bda174c7019bdc2ab1ec0 15000af016197044a69d640e1c1acfd91e49fe3d74678d5e5f94043b89f71cea359a1a596045530786ee16f1611ff3d1dbb069a2a41977293881008b1c46c6668e01b918bed3a5f89a83b43c77b107c0d8a4f7b65a3fc836ff7db17c622774cc21d1a5bf73e56b11d641ddd1dd8e173f56d9ce75304808ea972860c4e19817adde6e8164a660ec0cb9ddb1db80658ff19ca2f55f42a8695fb1ea73532948a3fa32a23c7aacb362a5b4356e6c52d698f93cbb0b28be 5ee74f583a2f344b",
	"GRP 5 6ee7f2dd78219696b17f9b5e7917554b 75 33140bf6faa74772 1500755a275929cd13a40cd07b14437fe8da018489 b3fdafe9b5c6f8d0",
	"GRP 6 98c9524d3daaef7039db91c1cd4af42a 9c b43e5ee3 19009ceb4163e00e8b108b91a5343cd0d5a84f435b 806330de1b86135b",
	"PKT 268f3156a623035c364315d2e5a9740b5ef24fb409883e172807ef7caaff0ebcd46fcb67b1fb882a3867e98d1f339c328863de393f 38 143 0 0 3156a623035c364315d2e5a9740b5ef24fb409883e172807ef7caaff0ebcd46fcb67b1fb882a3867e98d1f339c 328863de393f e42cd46523346dd9",
	"PKT 2b430089da805f7f50125d8e9a95122f464ec2bea0de9fed169fa608b159c5cff3408522d33e7a700a6af623d7a63103fdf4576d4380456e2f61a642d2ace07b558785d08c7ecfd6a3eb9e9a46e277d8a5ca3e559aecffd0ed37e39a118e06cf 43 128 67 55945 - 5f7f50125d8e9a95122f464ec2bea0de9fed169fa608b159c5cff3408522d33e7a700a6af623d7a63103fdf4576d4380456e2f61a642d2ace07b558785d08c7ecfd6a3eb9e9a46e277d8a5ca3e559aecffd0ed37e39a118e06cf d605e97226f193ac",
	"PKT 0626e755a5c9669af8804b45e5b4452dcc40f2412c499c42e82d0ce43b8876ed31baeffeb1e83e94e1d5aa79b532ce55827bc56ab60a1e37e1e1e856496b3a5ba2a976088c5a4c4681e0e3ec4e9d160330c35431e9f07b334a7a5f8d94f26a58c030a509295ce26239cb2c867a43c06051a4728ac5053fe7d4d7925aa3ac1bb41db275eca2eaf9 6 38 0 0 e755a5c9669af8804b45e5b4452dcc40f2412c499c42e82d0ce43b8876ed31baeffeb1e83e94 e1d5aa79b532ce55827bc56ab60a1e37e1e1e856496b3a5ba2a976088c5a4c4681e0e3ec4e9d160330c35431e9f07b334a7a5f8d94f26a58c030a509295ce26239cb2c867a43c06051a4728ac5053fe7d4d7925aa3ac1bb41db275eca2eaf9 a3e93cbf0bc174f8",
	"PKT 398018af80387eb00b80d14b25dcfdfa61d739dc6ec1b19664dbae56a6e4952c8c87753c8194d41ebd3109af3fd78599aa322a764577dbb6 57 128 0 0 - 18af80387eb00b80d14b25dcfdfa61d739dc6ec1b19664dbae56a6e4952c8c87753c8194d41ebd3109af3fd78599aa322a764577dbb6 7a1c9f599d84e6c8",
	"PKT 146603c7c45ac7c5437a034317e9c323193f740b12ceb55b3f8b68bc56b649cee52fa1b3d8423bbaf62bf527d810a5f618d7b476983e99253549dbe0d093f39aeb94253600a9ce7d4f727665030672f700c5e6426218a7490e7154ed6d468af0 20 90 870 50375 c7c5437a034317e9c323193f740b12ceb55b3f8b68bc56b649cee52fa1b3d8423bbaf62bf527d810a5f618d7b476983e99253549 dbe0d093f39aeb94253600a9ce7d4f727665030672f700c5e6426218a7490e7154ed6d468af0 da6534dae641f89a",
};

static bool checkVector(const std::string& line) {
	std::istringstream ss(line);
	std::string t;
	ss >> t;
	uint8_t out[255], h[8];
	if (t == "ADV") {
		std::string seed, pub, app, wire, hash; unsigned long ts;
		ss >> seed >> pub >> ts >> app >> wire >> hash;
		LocalIdentity id;
		bool ok = id.fromSeed(hx(seed).data()) && bytes(id.pubKey(), 32) == hx(pub);
		const std::vector<uint8_t> a = hx(app);
		Packet p;
		ok = ok && buildAdvertRaw(id, (uint32_t)ts, a.data(), (uint8_t)a.size(), true, p);
		const uint8_t n = p.encode(out, sizeof out);
		p.hash(h);
		ok = ok && bytes(out, n) == hx(wire) && bytes(h, 8) == hx(hash);
		Packet q;
		Advert adv;
		std::vector<uint8_t> w = hx(wire);
		ok = ok && q.decode(w.data(), (uint8_t)w.size()) && parseAdvert(q, &adv) && adv.timestamp == ts && bytes(adv.pubKey, 32) == hx(pub);
		w[45] ^= 0x01;   // a signature bit
		return ok && q.decode(w.data(), (uint8_t)w.size()) && !parseAdvert(q, &adv);
	}
	if (t == "GRP") {
		unsigned type; std::string key, kh, plain, wire, hash;
		ss >> type >> key >> kh >> plain >> wire >> hash;
		Channel ch;
		const std::vector<uint8_t> k = hx(key), pl = hx(plain);
		bool ok = ch.set(k.data(), (uint8_t)k.size()) && ch.hash == hx(kh)[0];
		Packet p;
		ok = ok && buildGroupPacket(ch, (PayloadType)type, pl.data(), (uint8_t)pl.size(), p);
		const uint8_t n = p.encode(out, sizeof out);
		p.hash(h);
		ok = ok && bytes(out, n) == hx(wire) && bytes(h, 8) == hx(hash);
		uint8_t back[200];
		const uint8_t bl = macThenDecrypt(ch.secret, p.payload + 1, (uint8_t)(p.payloadLen - 1), back);
		ok = ok && bl == (pl.size() + 15) / 16 * 16 && memcmp(back, pl.data(), pl.size()) == 0;
		for (uint8_t i = (uint8_t)pl.size(); i < bl; ++i) ok = ok && back[i] == 0;   // zero padded
		return ok;
	}
	std::string wire, path, payload, hash; unsigned hdr, plen, t0, t1;
	ss >> wire >> hdr >> plen >> t0 >> t1 >> path >> payload >> hash;
	const std::vector<uint8_t> w = hx(wire);
	Packet p;
	bool ok = p.decode(w.data(), (uint8_t)w.size()) && p.header == hdr && p.pathLen == plen && p.transport[0] == t0 && p.transport[1] == t1;
	ok = ok && bytes(p.path, p.pathBytes()) == hx(path) && bytes(p.payload, p.payloadLen) == hx(payload);
	p.hash(h);
	const uint8_t n = p.encode(out, sizeof out);
	return ok && bytes(h, 8) == hx(hash) && bytes(out, n) == w;
}

static void testVectors() {
	for (const char* v : kVectors) {
		const bool ok = checkVector(v);
		CHECK(ok);
		if (!ok) std::printf("  vector: %.50s\n", v);
	}
}

// Realistic packets from MeshCore's code: a channel message on "Public",
// a group datagram, and a repeater's advert that came through 2 hops.
static const char* kRealText = "150011c94bdc2ebadeef020e6c32e02e09892622315c2e23fc271ad7552914d7754a8fe203";
static const char* kRealData = "1900111c3a0823d046fe0217a63d329b805e6d0c9f";
static const char* kRealAdvert = "11025ac3e9f6d876b8299c19b859e01a1d14886c2a81fab3f4ca541ec0db2c1f000052c7842ccb6abd4c04b05c4adc77e1ea69d1cb3482a2c2c6eccc68bb8728f3c2e87dbebfe83827c92ac2e2bc9027512d4f1ff771a0e76730e76390469d4088c75df82171b30c92807e73024f80c6fa48696c6c746f7020525054";

static void testReal() {
	Channel pub;
	CHECK(pub.set(kPublicChannelKey, 16) && pub.hash == 0x11);
	GroupMessage m;
	Packet p;
	std::vector<uint8_t> w = hx(kRealText);
	CHECK(p.decode(w.data(), (uint8_t)w.size()) && p.payloadType() == payload_grp_txt && p.isFlood());
	CHECK(openGroup(p, &pub, 1, &m) && m.isText && m.timestamp == 1791700000u && m.textFlags == 0);
	CHECK(std::string((const char*)m.data) == "station-7: rain 2.3 mm" && m.len == 22);
	// The same message built here is byte for byte MeshCore's.
	Packet mine;
	CHECK(buildGroupText(pub, 1791700000u, "station-7", "rain 2.3 mm", mine));
	uint8_t out[255];
	CHECK(bytes(out, mine.encode(out, sizeof out)) == w);
	w = hx(kRealData);
	CHECK(p.decode(w.data(), (uint8_t)w.size()) && openGroup(p, &pub, 1, &m) && !m.isText);
	CHECK(m.dataType == 0xFF01 && m.len == 6 && bytes(m.data, 6) == hx("123456789abc"));
	const std::vector<uint8_t> d6 = hx("123456789abc");
	CHECK(buildGroupData(pub, 0xFF01, d6.data(), 6, mine) && bytes(out, mine.encode(out, sizeof out)) == w);
	// The advert: 2 hops, a repeater with a location and a name.
	w = hx(kRealAdvert);
	Advert a;
	CHECK(p.decode(w.data(), (uint8_t)w.size()) && p.hopCount() == 2 && p.path[0] == 0x5a && p.path[1] == 0xc3);
	CHECK(parseAdvert(p, &a) && a.timestamp == 1791700100u && a.dataValid);
	CHECK(bytes(a.pubKey, 32) == hx("e9f6d876b8299c19b859e01a1d14886c2a81fab3f4ca541ec0db2c1f000052c7"));
	CHECK(a.data.type == adv_repeater && a.data.hasLocation && a.data.latE6 == 41123456 && a.data.lonE6 == -87654321);
	CHECK(std::string(a.data.name) == "Hilltop RPT" && a.data.feature1 == 0);
	// Re-encoding the advert's data gives MeshCore's bytes.
	uint8_t app[kMaxAdvertData];
	CHECK(bytes(app, a.data.encode(app)) == bytes(a.appData, a.appLen));
	// The repeaters' path isn't signed: changing it keeps the signature good.
	w[2] = 0x77;
	CHECK(p.decode(w.data(), (uint8_t)w.size()) && parseAdvert(p, &a));
}

static void testEdges() {
	Packet p;
	uint8_t out[255];
	// decode: a reserved hash size, a path over 64 bytes, no payload, a
	// payload over 184, transport codes cut short.
	const std::vector<uint8_t> size4 = hx("11c1aabbccdd01");
	CHECK(!p.decode(size4.data(), (uint8_t)size4.size()));
	std::vector<uint8_t> longPath = {0x11, (uint8_t)(0x40 | 33)};
	longPath.resize(2 + 66 + 1, 0x01);
	CHECK(!p.decode(longPath.data(), (uint8_t)longPath.size()));
	const std::vector<uint8_t> noPayload = hx("1102aabb");
	CHECK(!p.decode(noPayload.data(), (uint8_t)noPayload.size()));
	std::vector<uint8_t> big = {0x11, 0x00};
	big.resize(2 + 185, 0x22);
	CHECK(!p.decode(big.data(), (uint8_t)big.size()));
	big.resize(2 + 184);
	CHECK(p.decode(big.data(), (uint8_t)big.size()) && p.payloadLen == 184);
	const std::vector<uint8_t> shortTransport = hx("1001020300");
	CHECK(!p.decode(shortTransport.data(), (uint8_t)shortTransport.size()));
	// Hop count 63 with 1 byte hashes is the most a path holds; 3 x 21 too.
	std::vector<uint8_t> hops = {0x11, 63};
	hops.resize(2 + 63 + 1, 0x09);
	CHECK(p.decode(hops.data(), (uint8_t)hops.size()) && p.hopCount() == 63 && p.pathBytes() == 63);
	CHECK(Packet::validPathLen(0x80 | 21) && !Packet::validPathLen(0x80 | 22) && !Packet::validPathLen(0x40 | 33));
	// encode refuses what decode would.
	p.begin(payload_grp_txt, route_flood);
	CHECK(p.encode(out, sizeof out) == 0);   // no payload
	p.payloadLen = 10;
	CHECK(p.encode(out, 10) == 0 && p.encode(out, 12) == 12);
	p.pathLen = 0xC0;
	CHECK(p.encode(out, sizeof out) == 0);

	// Advert data: names cut at a whole UTF-8 character within 32 bytes.
	AdvertData d;
	d.type = adv_sensor;
	d.hasLocation = true;
	d.latE6 = -33868800;
	d.lonE6 = 151209300;
	strcpy(d.name, "Weather \xC3\xA9t\xC3\xA9 station north");   // 2-byte characters, 27 bytes
	uint8_t app[kMaxAdvertData];
	const uint8_t n = d.encode(app);
	CHECK(n <= 32 && (app[0] & 0x90) == 0x90 && (app[0] & 0x0F) == adv_sensor);
	AdvertData back;
	CHECK(back.decode(app, n) && back.latE6 == d.latE6 && back.lonE6 == d.lonE6 && back.hasLocation);
	CHECK(strncmp(back.name, d.name, strlen(back.name)) == 0 && strlen(back.name) == (size_t)(n - 9));
	CHECK(utf8Prefix("ab\xC3\xA9", 3) == 2 && utf8Prefix("ab\xC3\xA9", 4) == 4 && utf8Prefix("\xE2\x82", 5) == 0);
	CHECK(utf8Prefix("\xFF", 4) == 0 && utf8Prefix("\xF0\x9F\x98\x80x", 4) == 4);
	CHECK(utf8Prefix("a\xC0\x80", 5) == 1 && utf8Prefix("a\xC1\xBF", 5) == 1 && utf8Prefix("a\xF5\x80\x80\x80", 9) == 1);   // overlong / out of range leads
	d.feature1 = 0x1234;
	d.name[0] = 0;
	const uint8_t n2 = d.encode(app);
	CHECK(n2 == 11 && (app[0] & 0x20) && !(app[0] & 0x80) && back.decode(app, n2) && back.feature1 == 0x1234);
	CHECK(!back.decode(app, 5));   // location cut short
	// A long name: cut to fit 32 bytes with the location, at a whole character.
	// "Weather " and ten 2-byte characters (28 bytes): 23 bytes are left after
	// flags and location, so 8 + 7 x 2 = 22 go, and the 8th character doesn't.
	strcpy(d.name, "Weather \xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9");
	d.feature1 = 0;
	const uint8_t n3 = d.encode(app);
	CHECK(n3 == 31 && back.decode(app, n3) && strlen(back.name) == 22);
	// decode never writes past name[], however long its input.
	uint8_t huge[200];
	memset(huge, 'n', sizeof huge);
	huge[0] = 0x80 | adv_chat;
	CHECK(back.decode(huge, sizeof huge) && strlen(back.name) == 32);

	// An advert with app data over 32 bytes: only 32 are signed and read.
	LocalIdentity id;
	uint8_t seed[32];
	for (int i = 0; i < 32; ++i) seed[i] = (uint8_t)(i + 1);
	CHECK(id.fromSeed(seed));
	Packet adv;
	CHECK(buildAdvert(id, 1000, d, false, adv) && adv.routeType() == route_direct && adv.hopCount() == 0);
	uint8_t raw32[kMaxAdvertData];
	memset(raw32, 'x', sizeof raw32);
	CHECK(buildAdvertRaw(id, 7, raw32, 32, true, adv));
	CHECK(!buildAdvertRaw(id, 7, raw32, 33, true, adv));
	adv.payload[adv.payloadLen++] = 'y';   // a 33rd byte, unsigned
	Advert a;
	CHECK(parseAdvert(adv, &a) && a.appLen == 32);
	adv.payloadLen = 99;   // shorter than key + time + signature
	CHECK(!parseAdvert(adv, &a));

	// Group messages: text cut to 160 bytes with its "sender: " prefix,
	// data at most 165 bytes, a plaintext MeshCore would drop is dropped.
	Channel ch;
	CHECK(ch.set(kPublicChannelKey, 16) && !ch.set(kPublicChannelKey, 15));
	std::string longText(300, 'q');
	Packet g;
	GroupMessage m;
	CHECK(buildGroupText(ch, 5, "bob", longText.c_str(), g) && openGroup(g, &ch, 1, &m));
	CHECK(m.len == 160 && std::string((char*)m.data) == "bob: " + std::string(155, 'q'));
	std::vector<uint8_t> d165(165, 0x5a);
	CHECK(buildGroupData(ch, 0xFF00, d165.data(), 165, g) && openGroup(g, &ch, 1, &m) && m.len == 165);
	CHECK(!buildGroupData(ch, 0xFF00, d165.data(), 166, g));
	uint8_t bad[5] = {1, 2, 3, 4, 0x04};   // text type 1 (a CLI command): not ours
	CHECK(buildGroupPacket(ch, payload_grp_txt, bad, 5, g) && !openGroup(g, &ch, 1, &m));
	uint8_t badLen[4] = {0x00, 0xFF, 20, 9};   // says 20 bytes, has 1 (and 12 of padding)
	CHECK(buildGroupPacket(ch, payload_grp_data, badLen, 4, g) && !openGroup(g, &ch, 1, &m));
	CHECK(!buildGroupPacket(ch, payload_advert, badLen, 4, g));
	// A wrong MAC, a wrong channel, a short packet.
	CHECK(buildGroupData(ch, 0xFF00, d165.data(), 10, g));
	g.payload[1] ^= 0x80;   // the MAC's first byte
	CHECK(!openGroup(g, &ch, 1, &m));
	g.payload[1] ^= 0x80;
	g.payload[2] ^= 0x01;   // its second
	CHECK(!openGroup(g, &ch, 1, &m));
	g.payload[2] ^= 0x01;
	CHECK(openGroup(g, &ch, 1, &m));
	Channel other;
	const uint8_t k2[16] = {9};
	CHECK(other.set(k2, 16) && !openGroup(g, &other, 1, &m));
	g.payloadLen = 3;
	CHECK(!openGroup(g, &ch, 1, &m));
	CHECK(macThenDecrypt(ch.secret, g.payload + 1, 2, out) == 0 && macThenDecrypt(ch.secret, g.payload + 1, 19, out) == 0);
	// Two channels whose keys hash to the same byte: the MAC picks the right one.
	uint8_t ka[16] = {0}, kb[16] = {0};
	Channel ca, cb;
	CHECK(ca.set(ka, 16));
	for (uint32_t i = 1; i < 100000; ++i) {
		memcpy(kb, &i, sizeof i);
		cb.set(kb, 16);
		if (cb.hash == ca.hash) break;
	}
	CHECK(cb.hash == ca.hash);
	const Channel both[2] = {ca, cb};
	CHECK(buildGroupText(cb, 9, "x", "for b", g) && openGroup(g, both, 2, &m) && m.channel == 1);
	CHECK(buildGroupText(ca, 9, "x", "for a", g) && openGroup(g, both, 2, &m) && m.channel == 0);
}

int main() {
	testVectors();
	testReal();
	testEdges();
	std::printf("meshcore_packet_test: %d checks, %d failures\n", g_checks, g_failures);
	return g_failures ? 1 : 0;
}
