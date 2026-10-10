// Host test for UbxProtocol.h, and for ublox_gps<TTransport> and
// xublox_gps against a simulated u-blox receiver on a simulated UART.
//
// g++ -std=c++17 -fno-exceptions -fno-rtti -Wall -Wextra -I../inc -I../../nmea/inc -Istub -I../../../iTransport/itransport/inc -I../../inc ublox_gps_test.cpp -o ublox_gps_test
#include <math.h>
#include <stdio.h>
#include <deque>
#include <string>
#include <vector>
#include "xublox_gps.h"   // pulls in ublox_gps.h

static int g_checks = 0, g_failures = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { ++g_failures; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static std::string sentence(const std::string& body) {
	uint8_t x = 0;
	for (char c : body) x ^= (uint8_t)c;
	char tail[8];
	snprintf(tail, sizeof tail, "*%02X\r\n", x);
	return "$" + body + tail;
}

// ---- the simulated receiver ----
//
// Generation 8 understands UBX-CFG-MSG and UBX-CFG-RATE and refuses
// UBX-CFG-VALSET; generation 10 the other way round. Each accepted
// message is applied and ACKed. Every measRate ms it sends the
// sentences switched on, at bytesPerMs (1 is 9600 baud, 12 is 115200).
struct Module {
	enum Gen { Gen8, Gen10 } gen = Gen8;
	bool     hearsUs = true;     // false: its RX is not connected
	bool     silent = false;     // sends nothing
	bool     fix = true;
	unsigned bytesPerMs = 12;
	uint32_t writeMs = 0;        // how long one of our writes takes to go out
	int      dropAcks = 0;       // lose this many ACKs

	// State, as configured. Defaults are u-blox's.
	uint8_t  rate[7] = {1, 1, 1, 1, 1, 1, 0};   // GGA GLL GSA GSV RMC VTG ZDA
	uint16_t measMs = 1000;

	// What happened.
	int  accepted = 0, refused = 0, badFrames = 0, writes = 0;
	std::vector<uint8_t> lastFrameIds;   // ids of the CFG messages received, in order

	iTransportRxSink* sink = nullptr;
	std::deque<uint8_t> out;
	const uint8_t* pending = nullptr;
	size_t   pendingLen = 0;
	uint32_t pendingDone = 0;
	bool     haveNow = false;
	uint32_t now = 0, lastEpoch = 0;
	ubx::Parser ubxIn;           // finds the frames; their payloads are read from raw
	std::vector<uint8_t> raw;

	bool write(const uint8_t* d, size_t n) {
		if (pending) return false;   // still sending the last one
		++writes;
		pending = d; pendingLen = n; pendingDone = now + writeMs;
		return true;
	}

	void receive(const uint8_t* d, size_t n) {   // our bytes arrive at the module
		if (!hearsUs) return;
		for (size_t i = 0; i < n; ++i) {
			raw.push_back(d[i]);
			ubx::Parser::Result r = ubxIn.feed(d[i]);
			if (r == ubx::Parser::Result::Bad) ++badFrames;
			if (r == ubx::Parser::Result::Frame) handle();
		}
	}

	void reply(uint8_t cls, uint8_t id, bool ok) {
		if (ok) ++accepted; else ++refused;
		if (ok && dropAcks > 0) { --dropAcks; return; }
		uint8_t f[10];
		const uint8_t p[2] = {cls, id};
		ubx::frame(f, ubx::kClassAck, ok ? ubx::kIdAckAck : ubx::kIdAckNak, p, 2);
		out.insert(out.end(), f, f + 10);
	}

	void handle() {
		const uint8_t cls = ubxIn.cls(), id = ubxIn.id();
		const uint16_t n = ubxIn.length();
		const uint8_t* p = raw.data() + raw.size() - 2 - n;   // the parser keeps only 32 bytes
		if (cls != ubx::kClassCfg) return;
		lastFrameIds.push_back(id);
		if (id == ubx::kIdCfgMsg && gen == Gen8 && n == 3 && p[0] == ubx::kClassNmea) {
			for (int s = 0; s < 7; ++s) if (ubx::nmeaMsgId((ubx::Nmea)s) == p[1]) rate[s] = p[2];
			reply(cls, id, true);
		} else if (id == ubx::kIdCfgRate && gen == Gen8 && n == 6) {
			measMs = (uint16_t)(p[0] | (p[1] << 8));
			reply(cls, id, true);
		} else if (id == ubx::kIdCfgValSet && gen == Gen10 && n >= 4 && p[0] == 0 && p[1] == ubx::kLayerRam) {
			// Key/value pairs; the key's size nibble says how long the value is.
			bool ok = true;
			for (uint16_t i = 4; i + 4 <= n && ok;) {
				const uint32_t key = (uint32_t)p[i] | ((uint32_t)p[i + 1] << 8) | ((uint32_t)p[i + 2] << 16) | ((uint32_t)p[i + 3] << 24);
				const unsigned size = (key >> 28) & 7;   // 2: one byte, 3: two
				i += 4;
				if (size == 2 && i + 1 <= n) {
					bool known = false;
					for (int s = 0; s < 7; ++s) if (ubx::nmeaUart1Key((ubx::Nmea)s) == key) { rate[s] = p[i]; known = true; }
					ok = known;
					i += 1;
				} else if (size == 3 && i + 2 <= n) {
					const uint16_t v = (uint16_t)(p[i] | (p[i + 1] << 8));
					if (key == ubx::kKeyRateMeas) measMs = v;
					else if (key != ubx::kKeyRateNav) ok = false;
					i += 2;
				} else {
					ok = false;
				}
			}
			reply(cls, id, ok);
		} else {
			reply(cls, id, false);
		}
	}

	void epoch() {
		const uint32_t s = now / 1000;
		char t[16];
		snprintf(t, sizeof t, "%02u%02u%02u.%02u", (unsigned)(s / 3600 % 24), (unsigned)(s / 60 % 60),
		         (unsigned)(s % 60), (unsigned)(now % 1000 / 10));
		const std::string T = t;
		std::string e;
		if (rate[0]) e += sentence(fix ? "GNGGA," + T + ",4717.11437,N,00833.91522,E,1,08,1.01,499.6,M,48.0,M,,"
		                               : "GNGGA," + T + ",,,,,0,00,99.99,,,,,,");
		if (rate[1]) e += sentence(fix ? "GNGLL,4717.11437,N,00833.91522,E," + T + ",A,A" : "GNGLL,,,,," + T + ",V,N");
		if (rate[2]) e += sentence(fix ? "GNGSA,A,3,23,29,07,08,09,18,26,,,,,,1.94,1.18,1.54,1"
		                               : "GNGSA,A,1,,,,,,,,,,,,,99.99,99.99,99.99,1");
		if (rate[3]) e += sentence("GPGSV,1,1,04,23,38,230,44,29,71,156,47,07,29,116,41,08,09,081,36,1");
		if (rate[4]) e += sentence(fix ? "GNRMC," + T + ",A,4717.11437,N,00833.91522,E,0.004,77.52,091026,,,A,V"
		                               : "GNRMC," + T + ",V,,,,,,,091026,,,N,V");
		if (rate[5]) e += sentence("GNVTG,77.52,T,,M,0.004,N,0.008,K,A");
		if (rate[6]) e += sentence("GNZDA," + T + ",09,10,2026,00,00");
		out.insert(out.end(), e.begin(), e.end());
	}

	void tick(uint32_t t) {
		now = t;
		if (!haveNow) { haveNow = true; lastEpoch = t; }
		if (pending && (int32_t)(t - pendingDone) >= 0) {
			receive(pending, pendingLen);
			pending = nullptr;
		}
		if ((uint32_t)(t - lastEpoch) >= measMs) {
			lastEpoch += measMs;
			if (!silent) epoch();
		}
		for (unsigned i = 0; i < bytesPerMs && !out.empty(); ++i) {
			const uint8_t b = out.front();
			out.pop_front();
			if (sink) sink->onByteReceived(b);
		}
	}
	// A power cycle: settings back to the defaults.
	void powerCycle() {
		const uint8_t d[7] = {1, 1, 1, 1, 1, 1, 0};
		for (int i = 0; i < 7; ++i) rate[i] = d[i];
		measMs = 1000;
		out.clear();
	}
};

// The transport the driver inherits: a UART wired to the module.
class SimUart : public iTransport {
public:
	explicit SimUart(Module* m) : _m(m) {}
	bool write(const uint8_t* d, size_t n) override { return _m->write(d, n); }
	void setRxSink(iTransportRxSink& s) override { _m->sink = &s; }
private:
	Module* _m;
};

typedef ublox_gps<SimUart> Gps;

// One main() a ms, the module ticking first.
template <typename G>
static void run(G& gps, Module& m, uint32_t& t, uint32_t ms, uint32_t every = 1) {
	for (uint32_t i = 0; i < ms; ++i, ++t) {
		m.tick(t);
		if (i % every == 0) gps.main(t);
	}
}

static const int32_t kLat = 472852395, kLon = 85652537;

static bool positionIs(Gps& g, int32_t lat, int32_t lon) {
	int32_t a = 0, b = 0;
	return g.position(&a, &b) && a == lat && b == lon;
}

// ---- tests ----

static void testListenOnly() {
	Module m;
	m.bytesPerMs = 1;   // 9600 baud, the default
	Gps gps(ublox_gps_param_t(ublox_config_t::None), &m);
	GnssData d;
	uint32_t t = 0;
	run(gps, m, t, 10);
	CHECK(!gps.getData(&d) && !gps.newData());           // nothing yet
	CHECK(isnan(d.altitudeM));
	run(gps, m, t, 1500);
	CHECK(m.writes == 0);                                 // never sends anything
	CHECK(gps.configStatus() == ublox_config_status_t::NotDone);
	CHECK(gps.newData() && gps.getData(&d) && !gps.newData());
	CHECK(positionIs(gps, kLat, kLon));
	CHECK(d.fixType == 3 && d.satelliteCount == 4 && fabsf(d.altitudeM - 499.6f) < 1e-3f);
	CHECK(gps.nmeaStats().checksumErrors == 0 && gps.nmeaStats().malformed == 0);
}

static void testLegacyGen8() {
	Module m;
	ublox_gps_param_t p(ublox_config_t::Legacy);
	p.measRateMs = 200;
	Gps gps(p, &m);
	uint32_t t = 0;
	run(gps, m, t, 2000);
	CHECK(gps.configStatus() == ublox_config_status_t::Done);
	CHECK(m.accepted == 8 && m.refused == 0 && m.badFrames == 0);   // 7 CFG-MSG + CFG-RATE
	CHECK(m.rate[0] == 1 && m.rate[1] == 0 && m.rate[2] == 1 && m.rate[3] == 1 &&
	      m.rate[4] == 1 && m.rate[5] == 0 && m.rate[6] == 0);
	CHECK(m.measMs == 200);
	// 5 Hz, four sentences each.
	const uint32_t before = gps.nmeaStats().sentences;
	run(gps, m, t, 1000);
	const uint32_t got = gps.nmeaStats().sentences - before;
	CHECK(got >= 18 && got <= 22);
	CHECK(positionIs(gps, kLat, kLon));
}

static void testValSetGen10() {
	Module m;
	m.gen = Module::Gen10;
	m.bytesPerMs = 4;   // 38400, u-blox 10's default
	ublox_gps_param_t p(ublox_config_t::ValSet);
	p.measRateMs = 100;
	p.sentences = ublox_sentence_bit(ubx::Nmea::GGA) | ublox_sentence_bit(ubx::Nmea::RMC);
	Gps gps(p, &m);
	uint32_t t = 0;
	run(gps, m, t, 1000);
	CHECK(gps.configStatus() == ublox_config_status_t::Done);
	CHECK(m.accepted == 1 && m.lastFrameIds.size() == 1 && m.lastFrameIds[0] == ubx::kIdCfgValSet);
	CHECK(m.rate[0] == 1 && m.rate[4] == 1 && m.rate[1] == 0 && m.rate[2] == 0 && m.rate[3] == 0 && m.rate[5] == 0);
	CHECK(m.measMs == 100);
	const uint32_t before = gps.nmeaStats().sentences;
	run(gps, m, t, 1000);
	const uint32_t got = gps.nmeaStats().sentences - before;
	CHECK(got >= 18 && got <= 22);   // 10 Hz, two sentences
	CHECK(positionIs(gps, kLat, kLon));
}

// The wrong generation: the receiver says NAK, and the driver carries
// on with what it sends by default.
static void testWrongGeneration() {
	{
		Module m;
		m.gen = Module::Gen10;
		Gps gps(ublox_gps_param_t(ublox_config_t::Legacy), &m);
		uint32_t t = 0;
		run(gps, m, t, 2000);
		CHECK(gps.configStatus() == ublox_config_status_t::Rejected);
		CHECK(m.refused == 1 && m.writes == 1);   // stops at the first NAK
		CHECK(positionIs(gps, kLat, kLon));
	}
	{
		Module m;
		Gps gps(ublox_gps_param_t(ublox_config_t::ValSet), &m);
		uint32_t t = 0;
		run(gps, m, t, 2000);
		CHECK(gps.configStatus() == ublox_config_status_t::Rejected);
		CHECK(positionIs(gps, kLat, kLon));
	}
}

// Its RX is not wired: no ACKs. Three tries at the first message, then
// carry on.
static void testNoAnswer() {
	Module m;
	m.hearsUs = false;
	Gps gps(ublox_gps_param_t(ublox_config_t::Legacy), &m);
	uint32_t t = 0;
	run(gps, m, t, 1400);
	CHECK(gps.configStatus() == ublox_config_status_t::NotDone);
	run(gps, m, t, 300);
	CHECK(gps.configStatus() == ublox_config_status_t::NoAnswer);
	CHECK(m.writes == 3 && gps.stats().cfgResends == 2);
	run(gps, m, t, 1000);
	CHECK(positionIs(gps, kLat, kLon));
}

static void testLostAck() {
	Module m;
	m.dropAcks = 1;
	Gps gps(ublox_gps_param_t(ublox_config_t::Legacy), &m);
	uint32_t t = 0;
	run(gps, m, t, 3000);
	CHECK(gps.configStatus() == ublox_config_status_t::Done);
	CHECK(gps.stats().cfgResends == 1 && m.accepted == 9);
}

// Our writes take a while to go out (a CFG-VALSET at 9600 baud takes
// about 75 ms), sent by the UART from the driver's buffer after write()
// has returned: the buffer must stay untouched until the ACK.
static void testSlowWrites() {
	Module m;
	m.writeMs = 150;
	ublox_gps_param_t p(ublox_config_t::Legacy);
	p.measRateMs = 500;
	Gps gps(p, &m);
	uint32_t t = 0;
	run(gps, m, t, 3000);
	CHECK(gps.configStatus() == ublox_config_status_t::Done);
	CHECK(m.badFrames == 0 && m.refused == 0 && gps.stats().cfgResends == 0);
	CHECK(m.rate[1] == 0 && m.rate[5] == 0 && m.measMs == 500);
}

static void testSilenceAndRecovery() {
	Module m;
	ublox_gps_param_t p(ublox_config_t::Legacy);
	Gps gps(p, &m);
	uint32_t t = 0;
	GnssData d;
	run(gps, m, t, 3000);
	CHECK(positionIs(gps, kLat, kLon) && gps.getData(&d));
	m.silent = true;                             // the last sentences came at about 2000
	run(gps, m, t, 1500);
	CHECK(positionIs(gps, kLat, kLon));          // not yet: silenceMs is 3000
	run(gps, m, t, 1000);
	CHECK(gps.state() == ublox_error_state);
	CHECK(!gps.position(&d.latitudeE7, &d.longitudeE7) && !gps.getData(&d));
	CHECK(!d.positionValid && !d.timeValid && isnan(d.hdop));
	// It comes back after a power cycle, at its defaults: configured again.
	m.powerCycle();
	m.silent = false;
	const int acceptedBefore = m.accepted;
	run(gps, m, t, 4000);
	CHECK(gps.configStatus() == ublox_config_status_t::Done);
	CHECK(m.accepted - acceptedBefore == 8 && m.rate[1] == 0);
	CHECK(positionIs(gps, kLat, kLon) && gps.state() == ublox_listening_state);
}

static void testNoFix() {
	Module m;
	m.fix = false;
	Gps gps(ublox_gps_param_t(ublox_config_t::None), &m);
	uint32_t t = 0;
	GnssData d;
	run(gps, m, t, 1500);
	CHECK(gps.getData(&d));                       // the receiver is talking...
	CHECK(!d.positionValid && d.fixType == 1);    // ...without a fix
	int32_t a, b;
	CHECK(!gps.position(&a, &b));
	CHECK(d.timeValid && d.dateValid && d.year == 2026);
	m.fix = true;
	run(gps, m, t, 1000);
	CHECK(positionIs(gps, kLat, kLon));
}

// main() not called for 200 ms at a time at 115200 baud: the ring
// overflows, sentences are lost, and nothing worse happens.
static void testRingOverflow() {
	Module m;
	m.bytesPerMs = 12;
	m.measMs = 100;                              // about 3.8 KB a second
	ublox_gps_param_t p(ublox_config_t::None);
	Gps gps(p, &m);
	uint32_t t = 0;
	run(gps, m, t, 3000, 200);
	CHECK(gps.stats().rxOverflows > 0);
	run(gps, m, t, 2000);
	CHECK(positionIs(gps, kLat, kLon));
}

static void testTickRollover() {
	Module m;
	Gps gps(ublox_gps_param_t(ublox_config_t::Legacy), &m);
	uint32_t t = 0xFFFFF000u;
	run(gps, m, t, 10000);
	CHECK(gps.configStatus() == ublox_config_status_t::Done);
	CHECK(positionIs(gps, kLat, kLon) && gps.state() == ublox_listening_state);
}

// xublox_gps: the thread sleeps, and the end of each line wakes it.
static void testRtosVariant() {
	Module m;
	g_rtos = SimRtos();
	int threadToken = 0;
	g_rtos.current = &threadToken;
	xublox_gps<SimUart> gps(ublox_gps_param_t(ublox_config_t::None), &m);
	uint32_t t = 0;
	for (uint32_t i = 0; i < 3000; ++i, ++t) { m.tick(t); gps.main(t); }
	CHECK(g_rtos.sets > 0);                       // wake() from the "interrupt"
	CHECK(g_rtos.blocks > 0);                     // sleep() really waited
	CHECK(g_rtos.lastTimeout <= ublox_max_sleep_ms);
	int32_t a, b;
	CHECK(gps.position(&a, &b) && a == kLat);
}

// ---- UBX framing (UbxProtocol.h) ----

static std::vector<uint8_t> bytes(const uint8_t* p, size_t n) { return std::vector<uint8_t>(p, p + n); }

static void testUbxBuilders() {
	uint8_t f[80];
	// Published commands: "disable GLL" and "5 Hz" for u-blox 6/7/8.
	size_t n = ubx::cfgMsg(f, ubx::Nmea::GLL, 0);
	const uint8_t gllOff[] = {0xB5, 0x62, 0x06, 0x01, 0x03, 0x00, 0xF0, 0x01, 0x00, 0xFB, 0x11};
	CHECK(bytes(f, n) == bytes(gllOff, sizeof gllOff));
	n = ubx::cfgRate(f, 200, 1, 1);
	const uint8_t rate5Hz[] = {0xB5, 0x62, 0x06, 0x08, 0x06, 0x00, 0xC8, 0x00, 0x01, 0x00, 0x01, 0x00, 0xDE, 0x6A};
	CHECK(bytes(f, n) == bytes(rate5Hz, sizeof rate5Hz));

	ubx::ValSet v(ubx::kLayerRam);
	v.addU1(ubx::nmeaUart1Key(ubx::Nmea::GGA), 1);
	v.addU2(ubx::kKeyRateMeas, 100);
	CHECK(v.ok() && v.length() == 4 + 5 + 6);
	n = v.build(f);
	const uint8_t head[] = {0xB5, 0x62, 0x06, 0x8A, 15, 0, 0x00, 0x01, 0x00, 0x00,
	                        0xBB, 0x00, 0x91, 0x20, 0x01, 0x01, 0x00, 0x21, 0x30, 100, 0};
	CHECK(n == 23 && bytes(f, sizeof head) == bytes(head, sizeof head));
	uint8_t a, b;
	ubx::checksum(f + 2, 4 + 15, &a, &b);
	CHECK(f[21] == a && f[22] == b);

	ubx::ValSet full(ubx::kLayerRam);
	for (int i = 0; i < 20; ++i) full.addU2(ubx::kKeyRateMeas, 1);
	CHECK(!full.ok() && full.length() <= ubx::ValSet::kMaxPayload);
}

static void testUbxParser() {
	ubx::Parser p;
	uint8_t ack[10];
	const uint8_t payload[2] = {0x06, 0x01};
	ubx::frame(ack, ubx::kClassAck, ubx::kIdAckAck, payload, 2);
	ubx::Parser::Result r = ubx::Parser::Result::NotMine;
	for (uint8_t c : ack) r = p.feed(c);
	CHECK(r == ubx::Parser::Result::Frame);
	bool acked = false;
	CHECK(p.isAckFor(0x06, 0x01, &acked) && acked);
	CHECK(!p.isAckFor(0x06, 0x08, &acked));

	ubx::frame(ack, ubx::kClassAck, ubx::kIdAckNak, payload, 2);
	for (uint8_t c : ack) r = p.feed(c);
	CHECK(r == ubx::Parser::Result::Frame && p.isAckFor(0x06, 0x01, &acked) && !acked);

	ack[8] ^= 1;   // spoil the checksum
	for (uint8_t c : ack) r = p.feed(c);
	CHECK(r == ubx::Parser::Result::Bad && p.errors() == 1);

	// Text is not its business; "B5" then text gives the text back.
	CHECK(p.feed('$') == ubx::Parser::Result::NotMine);
	CHECK(p.feed(0xB5) == ubx::Parser::Result::Consumed);
	CHECK(p.feed('$') == ubx::Parser::Result::NotMine && !p.inFrame());

	// A long frame (NAV-SAT size) is checked and its payload truncated.
	std::vector<uint8_t> big(200);
	for (size_t i = 0; i < big.size(); ++i) big[i] = (uint8_t)i;
	std::vector<uint8_t> fr(big.size() + 8);
	ubx::frame(fr.data(), 0x01, 0x35, big.data(), (uint16_t)big.size());
	for (uint8_t c : fr) r = p.feed(c);
	CHECK(r == ubx::Parser::Result::Frame && p.truncated() && p.length() == 200 && p.payload()[31] == 31);
	CHECK(!p.isAckFor(0x01, 0x35, &acked));
}

int main() {
	testUbxBuilders();
	testUbxParser();
	testListenOnly();
	testLegacyGen8();
	testValSetGen10();
	testWrongGeneration();
	testNoAnswer();
	testLostAck();
	testSlowWrites();
	testSilenceAndRecovery();
	testNoFix();
	testRingOverflow();
	testTickRollover();
	testRtosVariant();
	printf("ublox_gps_test: %d checks, %d failures\n", g_checks, g_failures);
	return g_failures ? 1 : 0;
}
