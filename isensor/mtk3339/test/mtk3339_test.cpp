// Host test for Pmtk.h, and for mtk3339<TTransport> and xmtk3339
// against a simulated MT3339 module on a simulated UART.
//
// g++ -std=c++17 -fno-exceptions -fno-rtti -Wall -Wextra -I../inc -I../../nmea/inc -I../../inc -Istub -I../../../iTransport/itransport/inc mtk3339_test.cpp -o mtk3339_test
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <deque>
#include <string>
#include <vector>
#include "xmtk3339.h"   // pulls in mtk3339.h

static int g_checks = 0, g_failures = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { ++g_failures; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static std::string sentence(const std::string& body) {
	uint8_t x = 0;
	for (char c : body) x ^= (uint8_t)c;
	char tail[8];
	snprintf(tail, sizeof tail, "*%02X\r\n", x);
	return "$" + body + tail;
}

// ---- the simulated module ----
//
// Reads our lines, checks their checksums, applies PMTK314/220/300 and
// PGCMD,33 and answers each PMTK command with $PMTK001,cmd,flag. Every
// update interval it sends the sentences switched on (each every
// rate'th output), at bytesPerMs (1 is 9600 baud).
struct Module {
	enum Vendor { GlobalTop, CDTop } vendor = GlobalTop;
	bool     hearsUs = true;
	bool     silent = false;
	bool     fix = true;
	unsigned bytesPerMs = 1;
	uint32_t writeMs = 0;            // how long one of our writes takes to go out
	int      dropAcks = 0;
	unsigned refuse = 0;             // a command it answers with flag 1 (unsupported)
	int      antennaKind = 3;        // GlobalTop numbering: 1 shorted, 2 internal, 3 external
	uint32_t firstAckDelayMs = 0;    // the first answer comes this late
	uint32_t ackDelayMs = 0;         // and the others this late
	std::vector<std::pair<uint32_t, std::string>> late;

	// State. MT3339 defaults: RMC, VTG, GGA, GSA, GSV at 1 Hz.
	uint8_t  rate[19] = {0, 1, 1, 1, 1, 1};
	uint16_t updateMs = 1000, fixMs = 1000;
	bool     antennaOn = false;

	// What happened.
	int writes = 0, acked = 0, refused = 0, badLines = 0;
	std::vector<std::string> commands;   // every line received, without $ and *hh

	iTransportRxSink* sink = nullptr;
	std::deque<uint8_t> out;
	const uint8_t* pending = nullptr;
	size_t   pendingLen = 0;
	uint32_t pendingDone = 0;
	bool     started = false;
	uint32_t now = 0, lastOut = 0, outputs = 0;
	uint32_t readyAt = 0;            // it ignores what arrives while it is still booting
	std::string line;

	void say(const std::string& s) { out.insert(out.end(), s.begin(), s.end()); }

	// Power on or reset: back to the defaults, and the start-up messages.
	void boot() {
		const uint8_t d[19] = {0, 1, 1, 1, 1, 1};
		for (int i = 0; i < 19; ++i) rate[i] = d[i];
		updateMs = fixMs = 1000;
		antennaOn = false;
		out.clear();
		say("$PMTK011,MTKGPS*08\r\n");
		say("$PMTK010,001*2E\r\n");
		readyAt = now + 20;
		line.clear();
	}

	bool write(const uint8_t* d, size_t n) {
		if (pending) return false;
		++writes;
		pending = d; pendingLen = n; pendingDone = now + writeMs;
		return true;
	}

	void ack(unsigned cmd, unsigned flag) {
		if (flag == 3) ++acked; else ++refused;
		if (flag == 3 && dropAcks > 0) { --dropAcks; return; }
		const std::string a = sentence("PMTK001," + std::to_string(cmd) + "," + std::to_string(flag));
		if (firstAckDelayMs) { late.push_back({now + firstAckDelayMs, a}); firstAckDelayMs = 0; return; }
		if (ackDelayMs) { late.push_back({now + ackDelayMs, a}); return; }
		say(a);
	}

	void handleLine(const std::string& l) {   // "$...*hh"
		const size_t star = l.find('*');
		if (l.size() < 4 || l[0] != '$' || star == std::string::npos || star + 3 != l.size()) { ++badLines; return; }
		const std::string body = l.substr(1, star - 1);
		uint8_t x = 0;
		for (char c : body) x ^= (uint8_t)c;
		if (strtoul(l.substr(star + 1).c_str(), nullptr, 16) != x) { ++badLines; return; }
		commands.push_back(body);
		std::vector<std::string> f;
		size_t p = 0;
		for (;;) { const size_t c = body.find(',', p); f.push_back(body.substr(p, c - p)); if (c == std::string::npos) break; p = c + 1; }
		if (f[0] == "PGCMD") { if (f.size() == 3 && f[1] == "33") antennaOn = f[2] == "1"; return; }
		if (f[0].compare(0, 4, "PMTK") != 0) return;
		const unsigned cmd = (unsigned)atoi(f[0].c_str() + 4);
		if (cmd == refuse) { ack(cmd, 1); return; }
		if (cmd == 314 && f.size() == 20) {
			for (int i = 0; i < 19; ++i) rate[i] = (uint8_t)atoi(f[i + 1].c_str());
			ack(cmd, 3);
		} else if (cmd == 220 && f.size() == 2) {
			const int ms = atoi(f[1].c_str());
			if (ms < 100 || ms > 10000) { ack(cmd, 2); return; }
			updateMs = (uint16_t)ms;
			ack(cmd, 3);
		} else if (cmd == 300 && f.size() == 6) {
			const int ms = atoi(f[1].c_str());
			if (ms < 200) { ack(cmd, 2); return; }   // it can't fix faster
			fixMs = (uint16_t)ms;
			ack(cmd, 3);
		} else {
			ack(cmd, 1);
		}
	}

	void receive(const uint8_t* d, size_t n) {
		if (!hearsUs || (int32_t)(now - readyAt) < 0) return;
		for (size_t i = 0; i < n; ++i) {
			const char c = (char)d[i];
			if (c == '\r') continue;
			if (c == '\n') { handleLine(line); line.clear(); } else line += c;
		}
	}

	void output() {
		++outputs;
		const uint32_t s = now / 1000;
		char t[16];
		snprintf(t, sizeof t, "%02u%02u%02u.%03u", (unsigned)(s / 3600 % 24), (unsigned)(s / 60 % 60),
		         (unsigned)(s % 60), (unsigned)(now % 1000));
		const std::string T = t;
		auto on = [&](int i) { return rate[i] && outputs % rate[i] == 0; };
		if (on(0)) say(sentence(fix ? "GPGLL,4042.6142,N,07400.4168,W," + T + ",A,A" : "GPGLL,,,,," + T + ",V,N"));
		if (on(1)) say(sentence(fix ? "GPRMC," + T + ",A,4042.6142,N,07400.4168,W,0.08,215.20,091026,,,A"
		                            : "GPRMC," + T + ",V,,,,,0.00,0.00,091026,,,N"));
		if (on(2)) say(sentence("GPVTG,215.20,T,,M,0.08,N,0.15,K,A"));
		if (on(3)) say(sentence(fix ? "GPGGA," + T + ",4042.6142,N,07400.4168,W,1,08,1.03,39.2,M,-34.2,M,,"
		                            : "GPGGA," + T + ",,,,,0,00,,,M,,M,,"));
		if (on(4)) say(sentence(fix ? "GPGSA,A,3,10,32,27,14,18,22,,,,,,,1.32,1.03,0.83" : "GPGSA,A,1,,,,,,,,,,,,,,,"));
		if (on(5)) say(sentence("GPGSV,1,1,03,10,63,137,17,32,61,050,,27,45,207,32"));
		if (on(17)) say(sentence("GPZDA," + T + ",09,10,2026,,"));
		if (antennaOn) {
			static const int cd[] = {0, 3, 1, 2};   // GlobalTop numbering to CDTop's
			say(vendor == GlobalTop ? sentence("PGTOP,11," + std::to_string(antennaKind))
			                        : sentence("PCD,11," + std::to_string(cd[antennaKind])));
		}
	}

	void tick(uint32_t t) {
		now = t;
		if (!started) { started = true; lastOut = t; boot(); }
		if (pending && (int32_t)(t - pendingDone) >= 0) { receive(pending, pendingLen); pending = nullptr; }
		for (size_t i = 0; i < late.size();) {
			if ((int32_t)(t - late[i].first) >= 0) { say(late[i].second); late.erase(late.begin() + (long)i); } else ++i;
		}
		if ((uint32_t)(t - lastOut) >= updateMs) {
			lastOut += updateMs;
			if (!silent) output();
		}
		for (unsigned i = 0; i < bytesPerMs && !out.empty(); ++i) {
			const uint8_t b = out.front();
			out.pop_front();
			if (sink) sink->onByteReceived(b);
		}
	}
};

class SimUart : public iTransport {
public:
	explicit SimUart(Module* m) : _m(m) {}
	bool write(const uint8_t* d, size_t n) override { return _m->write(d, n); }
	void setRxSink(iTransportRxSink& s) override { _m->sink = &s; }
private:
	Module* _m;
};

typedef mtk3339<SimUart> Gps;

template <typename G>
static void run(G& gps, Module& m, uint32_t& t, uint32_t ms, uint32_t every = 1) {
	for (uint32_t i = 0; i < ms; ++i, ++t) {
		m.tick(t);
		if (i % every == 0) gps.main(t);
	}
}

static const int32_t kLat = 407102367, kLon = -740069467;   // 40 42.6142 N, 74 00.4168 W

template <typename G>
static bool positionIs(G& g) {
	int32_t a = 0, b = 0;
	return g.position(&a, &b) && a == kLat && b == kLon;
}

static uint32_t bit(pmtk::Nmea s) { return mtk3339_sentence_bit(s); }

// ---- PMTK builders, against Adafruit's published commands ----

static void testPmtkBuilders() {
	char b[80];
	const uint8_t rmcgga[19] = {0, 1, 0, 1};
	pmtk::setNmeaOutput(b, sizeof b, rmcgga);
	CHECK(std::string(b) == "$PMTK314,0,1,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0*28\r\n");
	const uint8_t all[19] = {1, 1, 1, 1, 1, 1};
	pmtk::setNmeaOutput(b, sizeof b, all);
	CHECK(std::string(b) == "$PMTK314,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0*28\r\n");
	pmtk::setUpdateRate(b, sizeof b, 1000);  CHECK(std::string(b) == "$PMTK220,1000*1F\r\n");
	pmtk::setUpdateRate(b, sizeof b, 200);   CHECK(std::string(b) == "$PMTK220,200*2C\r\n");
	pmtk::setUpdateRate(b, sizeof b, 100);   CHECK(std::string(b) == "$PMTK220,100*2F\r\n");
	pmtk::setUpdateRate(b, sizeof b, 10000); CHECK(std::string(b) == "$PMTK220,10000*2F\r\n");
	pmtk::setFixInterval(b, sizeof b, 1000); CHECK(std::string(b) == "$PMTK300,1000,0,0,0,0*1C\r\n");
	pmtk::setFixInterval(b, sizeof b, 200);  CHECK(std::string(b) == "$PMTK300,200,0,0,0,0*2F\r\n");
	pmtk::antennaStatus(b, sizeof b, true);  CHECK(std::string(b) == "$PGCMD,33,1*6C\r\n");
	pmtk::antennaStatus(b, sizeof b, false); CHECK(std::string(b) == "$PGCMD,33,0*6D\r\n");
	// Too small a buffer: nothing, rather than a cut-off sentence.
	CHECK(pmtk::setUpdateRate(b, 10, 1000) == 0);
	CHECK(pmtk::sentence(b, sizeof b, "PMTK605") == 13 && std::string(b) == "$PMTK605*31\r\n");   // Adafruit's PMTK_Q_RELEASE
}

// NmeaParser hands proprietary sentences over through address()/field().
static void testProprietaryFields() {
	NmeaParser p;
	NmeaParser::Sentence r = NmeaParser::Sentence::None;
	for (char c : std::string("$PMTK001,314,3*36\r\n")) { NmeaParser::Sentence x = p.feed((uint8_t)c); if (x != NmeaParser::Sentence::None) r = x; }
	CHECK(r == NmeaParser::Sentence::Other);
	const char* s; size_t n;
	p.address(&s, &n);
	CHECK(std::string(s, n) == "PMTK001" && p.fieldCount() == 3);
	CHECK(p.field(1, &s, &n) && std::string(s, n) == "314");
	CHECK(p.field(2, &s, &n) && std::string(s, n) == "3");
	CHECK(!p.field(3, &s, &n) && n == 0);
	// A new sentence starting: the old fields are gone.
	p.feed('$');
	CHECK(p.fieldCount() == 0);
}

// ---- the driver ----

static void testListenOnly() {
	Module m;
	Gps gps(mtk3339_param_t(false), &m);
	uint32_t t = 0;
	GnssData d;
	run(gps, m, t, 10);
	CHECK(!gps.getData(&d) && isnan(d.hdop));
	run(gps, m, t, 1500);
	CHECK(m.writes == 0 && gps.configStatus() == mtk3339_config_status_t::NotDone);
	CHECK(gps.newData() && gps.getData(&d) && positionIs(gps));
	CHECK(d.fixType == 3 && d.satelliteCount == 3 && fabsf(d.altitudeM - 39.2f) < 1e-3f);
	CHECK(gps.antenna() == mtk3339_antenna_t::Unknown);
	CHECK(gps.stats().restarts == 1);   // it saw the module start
	CHECK(gps.nmeaStats().checksumErrors == 0 && gps.nmeaStats().malformed == 0);
}

static void testConfigureDefaults() {
	Module m;
	Gps gps(mtk3339_param_t(true), &m);
	uint32_t t = 0;
	run(gps, m, t, 1000);
	CHECK(gps.configStatus() == mtk3339_config_status_t::Done);
	CHECK(m.acked == 3 && m.badLines == 0 && m.commands.size() == 3);
	CHECK(m.writes == 4 && gps.stats().restarts == 1);   // the first went while it was booting
	CHECK(m.rate[0] == 0 && m.rate[1] == 1 && m.rate[2] == 0 && m.rate[3] == 1 && m.rate[4] == 1 && m.rate[5] == 1);
	CHECK(m.updateMs == 1000 && m.fixMs == 1000);
	run(gps, m, t, 2000);
	CHECK(positionIs(gps));
}

static void testFiveHz() {
	Module m;
	mtk3339_param_t p(true);
	p.updateMs = 200;
	p.sentences = bit(pmtk::Nmea::RMC) | bit(pmtk::Nmea::GGA);
	Gps gps(p, &m);
	uint32_t t = 0;
	run(gps, m, t, 2000);
	CHECK(gps.configStatus() == mtk3339_config_status_t::Done);
	CHECK(m.updateMs == 200 && m.fixMs == 200);
	const uint32_t before = gps.nmeaStats().sentences;
	run(gps, m, t, 1000);
	const uint32_t got = gps.nmeaStats().sentences - before;
	CHECK(got >= 9 && got <= 11);   // RMC + GGA, 5 a second, at 9600 baud
	CHECK(positionIs(gps) && gps.stats().rxOverflows == 0);
}

static void testTenHz() {
	Module m;
	m.bytesPerMs = 12;   // 115200
	mtk3339_param_t p(true);
	p.updateMs = 100;
	p.sentences = bit(pmtk::Nmea::RMC);
	Gps gps(p, &m);
	uint32_t t = 0;
	run(gps, m, t, 2000);
	CHECK(gps.configStatus() == mtk3339_config_status_t::Done);
	CHECK(m.updateMs == 100 && m.fixMs == 200);   // fixes never faster than 5 Hz
	const uint32_t before = gps.nmeaStats().sentences;
	run(gps, m, t, 1000);
	const uint32_t got = gps.nmeaStats().sentences - before;
	CHECK(got >= 9 && got <= 11);
	GnssData d;
	CHECK(gps.getData(&d) && d.positionValid && d.millisecond % 100 == 0);
}

static void testZda() {
	Module m;
	mtk3339_param_t p(true);
	p.sentences = bit(pmtk::Nmea::RMC) | bit(pmtk::Nmea::ZDA);
	Gps gps(p, &m);
	uint32_t t = 0;
	run(gps, m, t, 2500);
	CHECK(m.rate[17] == 1 && m.rate[3] == 0);
	GnssData d;
	CHECK(gps.getData(&d) && d.dateValid && d.year == 2026 && d.month == 10 && d.day == 9);
}

static void testRejected() {
	Module m;
	m.refuse = 300;
	Gps gps(mtk3339_param_t(true), &m);
	uint32_t t = 0;
	run(gps, m, t, 2000);
	CHECK(gps.configStatus() == mtk3339_config_status_t::Rejected);
	CHECK(gps.configFlag() == pmtk::AckFlag::Unsupported && gps.configCommand() == 300);
	CHECK(m.acked == 2 && m.refused == 1);
	CHECK(positionIs(gps));   // still running on what it sends
}

// The first answer comes after the timeout: the command is sent again,
// and its second answer comes while the driver waits for the next
// command. Each answer must be matched to its own command: here PMTK300
// is refused, and the late PMTK314 answers must not stand in for it.
static void testLateAck() {
	Module m;
	m.firstAckDelayMs = 1050;   // just after the 1000 ms timeout
	m.ackDelayMs = 100;
	m.refuse = 300;
	Gps gps(mtk3339_param_t(true), &m);
	uint32_t t = 0;
	run(gps, m, t, 6000);
	CHECK(gps.stats().cfgResends >= 1);
	CHECK(gps.configStatus() == mtk3339_config_status_t::Rejected && gps.configCommand() == 300);
	CHECK(m.rate[2] == 0 && m.updateMs == 1000);
}

static void testNoAnswer() {
	Module m;
	m.hearsUs = false;
	Gps gps(mtk3339_param_t(true), &m);
	uint32_t t = 0;
	run(gps, m, t, 2900);
	CHECK(gps.configStatus() == mtk3339_config_status_t::NotDone);
	run(gps, m, t, 300);
	CHECK(gps.configStatus() == mtk3339_config_status_t::NoAnswer);
	CHECK(m.writes == 4 && gps.stats().cfgResends == 2);   // one before its start-up message, three after
	CHECK(positionIs(gps));
}

static void testLostAck() {
	Module m;
	m.dropAcks = 1;
	Gps gps(mtk3339_param_t(true), &m);
	uint32_t t = 0;
	run(gps, m, t, 3000);
	CHECK(gps.configStatus() == mtk3339_config_status_t::Done);
	CHECK(gps.stats().cfgResends == 1 && m.acked == 4);
}

// The module resets while running: it says so, and is configured again.
static void testModuleRestart() {
	Module m;
	mtk3339_param_t p(true);
	p.updateMs = 500;
	Gps gps(p, &m);
	uint32_t t = 0;
	run(gps, m, t, 2000);
	CHECK(gps.configStatus() == mtk3339_config_status_t::Done && m.updateMs == 500);
	CHECK(gps.stats().restarts == 1);
	m.boot();
	CHECK(m.updateMs == 1000 && m.rate[2] == 1);
	run(gps, m, t, 1000);
	CHECK(gps.stats().restarts == 2);
	CHECK(gps.configStatus() == mtk3339_config_status_t::Done);
	CHECK(m.updateMs == 500 && m.rate[2] == 0 && m.acked == 6);
	CHECK(positionIs(gps));
}

static void testAntenna() {
	{
		Module m;
		mtk3339_param_t p(true);
		p.antennaStatus = true;
		Gps gps(p, &m);
		uint32_t t = 0;
		run(gps, m, t, 2000);
		CHECK(m.antennaOn && m.commands[0] == "PGCMD,33,1");
		CHECK(gps.configStatus() == mtk3339_config_status_t::Done);
		CHECK(gps.antenna() == mtk3339_antenna_t::External);
		m.antennaKind = 1;
		run(gps, m, t, 1100);
		CHECK(gps.antenna() == mtk3339_antenna_t::Shorted);
	}
	{
		Module m;
		m.vendor = Module::CDTop;
		m.antennaKind = 2;   // internal, sent as $PCD,11,1
		mtk3339_param_t p(true);
		p.antennaStatus = true;
		Gps gps(p, &m);
		uint32_t t = 0;
		run(gps, m, t, 2000);
		CHECK(gps.antenna() == mtk3339_antenna_t::Internal);
	}
}

static void testSilenceAndRecovery() {
	Module m;
	mtk3339_param_t p(true);
	p.antennaStatus = true;
	Gps gps(p, &m);
	uint32_t t = 0;
	GnssData d;
	run(gps, m, t, 3000);
	CHECK(positionIs(gps) && gps.antenna() == mtk3339_antenna_t::External);
	m.silent = true;                       // last output at about 2000
	run(gps, m, t, 1500);
	CHECK(positionIs(gps));
	run(gps, m, t, 1000);
	CHECK(gps.state() == mtk3339_error_state);
	CHECK(!gps.getData(&d) && !d.positionValid && gps.antenna() == mtk3339_antenna_t::Unknown);
	// Back after a power cycle.
	m.silent = false;
	m.boot();
	const int ackedBefore = m.acked;
	run(gps, m, t, 4000);
	CHECK(gps.configStatus() == mtk3339_config_status_t::Done && m.acked - ackedBefore >= 3);
	CHECK(positionIs(gps) && gps.state() == mtk3339_listening_state);
	CHECK(gps.antenna() == mtk3339_antenna_t::External);
}

static void testNoFix() {
	Module m;
	m.fix = false;
	Gps gps(mtk3339_param_t(false), &m);
	uint32_t t = 0;
	GnssData d;
	run(gps, m, t, 1500);
	CHECK(gps.getData(&d) && !d.positionValid && d.fixType == 1);
	int32_t a, b;
	CHECK(!gps.position(&a, &b));
	m.fix = true;
	run(gps, m, t, 1100);
	CHECK(positionIs(gps));
}

static void testRingOverflow() {
	Module m;
	m.bytesPerMs = 12;
	Gps gps(mtk3339_param_t(false), &m);
	uint32_t t = 0;
	run(gps, m, t, 1);
	m.updateMs = 100;   // after its boot, which sets the defaults
	run(gps, m, t, 3000, 200);
	CHECK(gps.stats().rxOverflows > 0);
	run(gps, m, t, 2000);
	CHECK(positionIs(gps));
}

static void testTickRollover() {
	Module m;
	Gps gps(mtk3339_param_t(true), &m);
	uint32_t t = 0xFFFFF000u;
	run(gps, m, t, 10000);
	CHECK(gps.configStatus() == mtk3339_config_status_t::Done);
	CHECK(positionIs(gps) && gps.state() == mtk3339_listening_state);
}

static void testRtosVariant() {
	Module m;
	g_rtos = SimRtos();
	int threadToken = 0;
	g_rtos.current = &threadToken;
	xmtk3339<SimUart> gps(mtk3339_param_t(true), &m);
	uint32_t t = 0;
	for (uint32_t i = 0; i < 3000; ++i, ++t) { m.tick(t); gps.main(t); }
	CHECK(g_rtos.sets > 0 && g_rtos.blocks > 0);
	CHECK(g_rtos.lastTimeout <= mtk3339_max_sleep_ms);
	CHECK(gps.configStatus() == mtk3339_config_status_t::Done && positionIs(gps));
}

int main() {
	testPmtkBuilders();
	testProprietaryFields();
	testListenOnly();
	testConfigureDefaults();
	testFiveHz();
	testTenHz();
	testZda();
	testRejected();
	testNoAnswer();
	testLateAck();
	testLostAck();
	testModuleRestart();
	testAntenna();
	testSilenceAndRecovery();
	testNoFix();
	testRingOverflow();
	testTickRollover();
	testRtosVariant();
	printf("mtk3339_test: %d checks, %d failures\n", g_checks, g_failures);
	return g_failures ? 1 : 0;
}
