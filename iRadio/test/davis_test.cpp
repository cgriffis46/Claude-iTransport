/*
 * davis_test.cpp
 *
 * Host test for the Davis protocol, DavisSchedule and DavisWeather: pure
 * logic, no radio. The schedule is run against simulated stations and an
 * ideal receiver that hears a station exactly when it listens on the
 * station's channel as it sends.
 *
 *   g++ -std=c++17 -Wall -Wextra -I../davis/inc davis_test.cpp \
 *       ../davis/src/DavisProtocol.cpp ../davis/src/DavisSchedule.cpp ../davis/src/DavisWeather.cpp -o davis_test
 */

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
#include "DavisProtocol.h"
#include "DavisSchedule.h"
#include "DavisWeather.h"

using namespace DAVIS;

static int g_failures = 0;
static void check(bool ok, const char* what) {
	std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok) ++g_failures;
}
static bool near(float a, float b, float tol) { return std::fabs(a - b) <= tol; }

static void protocol() {
	std::printf("protocol\n");
	const uint8_t check9[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
	check(crc16(check9, 9) == 0x31C3, "CRC-16-CCITT (XMODEM) of \"123456789\" is 0x31C3");
	check(reverseBits(0x01) == 0x80 && reverseBits(0xCB) == 0xD3 && reverseBits(reverseBits(0x5A)) == 0x5A, "bit reversal");

	check(bandChannels(davis_band_us) == 51 && bandChannels(davis_band_eu) == 5, "51 US channels, 5 EU");
	check(channelHz(davis_band_us, 0) == 911413818u, "US channel 0: 0xE3DA7C x 61.035 Hz = 911.413818 MHz");
	bool inBand = true;
	for (uint8_t c = 0; c < 51; ++c) {
		const uint32_t f = channelHz(davis_band_us, c);
		if (f < 902000000u || f > 928000000u) inBand = false;
	}
	check(inBand, "every US channel in 902-928 MHz");
	check(channelHz(davis_band_eu, 4) / 1000000 == 868, "EU on 868 MHz");
	check(bandFrf(davis_band_us, 99) == bandFrf(davis_band_us, 0), "an out of range channel is channel 0");
	check(intervalSixteenths(0) == 41000 && intervalSixteenths(7) == 48000, "intervals: 2.5625 s for id 0, 3 s for id 7");

	uint8_t d[6] = {0x80, 7, 128, 0x2D, 0x50, 0x00};
	uint8_t p[kPacketLen];
	buildPacket(d, p);
	check(checkCrc(p) == davis_crc_direct && p[8] == 0xFF && p[9] == 0xFF, "a built packet passes as direct");
	uint8_t rep[kPacketLen];
	std::memcpy(rep, p, sizeof rep);
	rep[8] = 0x12; rep[9] = 0x34;
	const uint16_t rc = crc16(rep + 8, 2, crc16(rep, 6));
	rep[6] = (uint8_t)(rc >> 8); rep[7] = (uint8_t)rc;
	check(checkCrc(rep) == davis_crc_repeater, "CRC over bytes 0-5 and 8-9: repeated");
	p[3] ^= 1;
	check(checkCrc(p) == davis_crc_bad, "one bit flipped: bad");
	uint8_t zero[kPacketLen] = {0};
	check(checkCrc(zero) == davis_crc_bad, "all zero: bad, though its CRC is 0");

	DavisReading r;
	decode(d, false, r);
	check(r.type == DAVIS_MSG_TEMP && r.station == 0 && !r.batteryLow && near(r.value, 72.5f, 0.01f), "temperature 0x2D50: 72.5 F");
	check(r.windSpeedMph == 7 && near(r.windDirDeg, 9 + 127 * 342.0f / 255.0f, 0.01f), "wind 7 mph, VP2 direction scale");
	decode(d, true, r);
	check(near(r.windDirDeg, 256 * 360.0f / 512.0f, 0.01f), "Vue direction scale: 9 bits");
	uint8_t neg[6] = {0x8B, 0, 0, 0xFF, 0x60, 0};		// -16 tenths/16... = -0.1 F, battery low, id 3
	decode(neg, false, r);
	check(r.station == 3 && r.batteryLow && std::isnan(r.windDirDeg), "station 3, battery low, no anemometer");
	check(std::isnan(r.value), "temperature byte 3 0xFF: no sensor");
	uint8_t cold[6] = {0x80, 0, 0, 0xFE, 0x70, 0};		// 0xFE70 = -400 -> -2.5 F
	decode(cold, false, r);
	check(near(r.value, -2.5f, 0.01f), "below zero: -2.5 F");
	uint8_t hum[6] = {0xA0, 0, 0, 0xC8, 0x10, 0};		// 0x1C8 = 456 -> 45.6 %
	decode(hum, false, r);
	check(near(r.value, 45.6f, 0.01f), "humidity 45.6 %");
	uint8_t hum0[6] = {0xA0, 0, 0, 0, 0, 0};
	decode(hum0, false, r);
	check(std::isnan(r.value), "humidity 0: no sensor");
	uint8_t uv[6] = {0x40, 0, 0, 0x0C, 0x80, 0};		// 0x0C80 >> 6 = 50 -> 1.0
	decode(uv, false, r);
	check(near(r.value, 1.0f, 0.001f), "UV index 1.0");
	uint8_t sol[6] = {0x60, 0, 0, 0xFF, 0x80, 0};		// 0x3FE: none
	decode(sol, false, r);
	check(std::isnan(r.value), "solar 0x3FE: no sensor");
	uint8_t rain[6] = {0xE0, 0, 0, 0x85, 0, 0};
	decode(rain, false, r);
	check(near(r.value, 5.0f, 0.001f), "rain counter 0x85: 5 (top bit masked)");
	uint8_t rain80[6] = {0xE0, 0, 0, 0x80, 0, 0};
	decode(rain80, false, r);
	check(std::isnan(r.value), "rain counter 0x80: no gauge");
	uint8_t light[6] = {0x50, 0, 0, 0x2C, 0x41, 0};		// light: 0x12C = 300 s... bits 5-4 of byte 4 = 0
	decode(light, false, r);
	check(near(r.value, 0x2C, 0.001f), "light rain: 44 s between tips");
	uint8_t heavy[6] = {0x50, 0, 0, 0x50, 0x00, 0};		// heavy: 0x050 >> 4 = 5 s
	decode(heavy, false, r);
	check(near(r.value, 5.0f, 0.001f), "heavy rain: the value shifted, 5 s");
	uint8_t dry[6] = {0x50, 0, 0, 0xFF, 0x30, 0};
	decode(dry, false, r);
	check(std::isnan(r.value), "0x3FF: not raining");
	uint8_t gust[6] = {0x90, 0, 0, 15, 0, 0x30};
	decode(gust, false, r);
	check(near(r.value, 15, 0.001f) && r.gustIndex == 3, "gust 15 mph, index 3");
	uint8_t cap[6] = {0x20, 0, 0, 0x4B, 0x40, 0};		// (0x4B << 2 | 1) = 301 -> 3.01 V
	decode(cap, true, r);
	check(near(r.value, 3.01f, 0.001f), "Vue supercap 3.01 V");
}

// An ideal receiver following the schedule, and stations sending.
struct Tx {
	uint8_t id;
	uint8_t channel;
	uint32_t next;			// ticks
	bool on;
	uint32_t sent;
};

// The world steps in ms; the schedule's clock counts tps ticks a second
// (16000: the ms tick x 16; 32768 or 256: the RTC).
struct World {
	DavisSchedule s;
	std::vector<Tx> tx;
	uint32_t now = 0;		// ms
	uint32_t tps = 16000;
	int64_t clockOffset = 0;	// the clock set away from true time
	DavisSchedule::Plan plan = {0, 0, -1, false};
	uint32_t heard[8] = {0};
	uint8_t nCh = 51;

	uint32_t trueTicks(uint32_t ms) const { return (uint32_t)((uint64_t)ms * tps / 1000u); }
	uint32_t clock(uint32_t ms) const { return (uint32_t)((int64_t)trueTicks(ms) + clockOffset); }

	void start(uint32_t t0, uint8_t mask, davis_band_t band = davis_band_us, uint32_t ticksPerSecond = 16000) {
		now = t0;
		tps = ticksPerSecond;
		nCh = bandChannels(band);
		s.begin(band, mask, clock(now), tps);
		plan = s.plan(clock(now));
	}
	void add(uint8_t id, uint8_t channel, uint32_t firstMs) { tx.push_back({id, channel, trueTicks(firstMs), true, 0}); }
	// Runs ms milliseconds. Returns packets heard.
	uint32_t run(uint32_t ms) {
		uint32_t n = 0;
		for (uint32_t i = 0; i < ms; ++i, ++now) {
			bool got = false;
			for (Tx& t : tx) {
				while ((int32_t)(trueTicks(now) - t.next) >= 0) {
					if (t.on) {
						++t.sent;
						if (plan.channel == t.channel && s.onPacket(t.id, t.channel, clock(now))) {
							++heard[t.id];
							++n;
							got = true;
						}
					}
					t.next += intervalTicks(t.id, tps);
					t.channel = (uint8_t)((t.channel + 1) % nCh);
				}
			}
			if (got || s.expired(plan, clock(now))) plan = s.plan(clock(now));
		}
		return n;
	}
};

static void schedule() {
	std::printf("DavisSchedule\n");
	{
		World w;
		w.start(1000, 0x01);
		w.add(0, 23, 1700);
		const uint32_t cycleMs = 51 * 2563;
		uint32_t t = 0;
		while (!w.s.station(0).synced && t < 2 * cycleMs) { w.run(100); t += 100; }
		check(w.s.station(0).synced && t <= cycleMs + 3000, "one station: found within one cycle (131 s)");
		const uint32_t before = w.heard[0];
		w.run(600000);
		const uint32_t expected = 600000 * 16 / intervalSixteenths(0);
		check(w.heard[0] - before >= expected - 1 && w.s.station(0).missed == 0, "then 10 minutes: every packet, none missed");
		check(w.plan.station == 0 && !w.plan.discovery, "tuned for station 0, not discovering");
	}
	{
		World w;
		w.start(0, 0x01);
		w.add(0, 0, 500);
		w.run(140000);
		const uint32_t h = w.heard[0];
		w.tx[0].on = false;
		w.run(30000);							// about 12 packets
		check(w.s.station(0).synced && w.s.station(0).missed >= 11 && w.s.station(0).missed <= 12, "silent 30 s: about 12 missed, still synced");
		w.tx[0].on = true;
		w.run(10000);
		check(w.heard[0] > h && w.s.station(0).lostInARow == 0, "back: heard again at once, on the channel it hopped to");
		// 45 in a row: the time they were due has moved on by 45 exact
		// intervals, so the first one back is heard (a 2562 ms interval
		// would be 22 ms out by now, past the 20 ms window).
		w.tx[0].on = false;
		w.run(45 * 2562 + 1000);
		const uint32_t missed = w.s.station(0).missed, h2 = w.heard[0];
		w.tx[0].on = true;
		w.run(2600);
		check(w.heard[0] == h2 + 1 && w.s.station(0).missed == missed, "after 45 missed, the very next packet is heard");
		w.tx[0].on = false;
		w.run(150000);							// past 50 misses
		check(!w.s.station(0).synced && w.s.station(0).resyncs == 1, "silent 150 s: lost after 50 misses");
		check(w.s.discoveryStation() == 0, "and looked for again");
		w.tx[0].on = true;
		w.run(140000);
		check(w.s.station(0).synced, "found again within a cycle");
	}
	{
		World w;
		w.start(0, 0x05);						// ids 0 and 2
		w.add(0, 10, 300);
		w.add(2, 40, 1300);
		w.run(300000);
		check(w.s.station(0).synced && w.s.station(2).synced, "two stations: both found");
		const uint32_t sent0 = w.tx[0].sent, sent2 = w.tx[1].sent, h0 = w.heard[0], h2 = w.heard[2];
		w.run(600000);
		const uint32_t s0 = w.tx[0].sent - sent0, s2 = w.tx[1].sent - sent2;
		const uint32_t g0 = w.heard[0] - h0, g2 = w.heard[2] - h2;
		std::printf("        10 min: station 1 %u/%u, station 3 %u/%u\n", g0, s0, g2, s2);
		check(g0 * 100 >= s0 * 95 && g2 * 100 >= s2 * 95, "and at least 95 % of each one's packets (only clashes lost)");
		check(w.s.station(0).synced && w.s.station(2).synced, "both still synced");
	}
	{
		World w;
		w.start(0, 0x01);
		w.add(0, 5, 100);
		w.add(1, 5, 200);						// not active
		w.run(200000);
		check(w.heard[1] == 0 && !w.s.station(1).synced && w.s.station(1).packets == 0, "an inactive station is ignored");
		w.s.setStationActive(1, true, w.now);
		w.run(160000);
		check(w.s.station(1).synced && w.s.station(0).synced, "made active: found too, station 0 kept");
		w.s.setStationActive(0, false, w.now);
		check(!w.s.station(0).synced && w.s.station(0).active == false, "made inactive: dropped");
	}
	{
		World w;
		w.start(0xFFFF0000u, 0x80);				// rollover of the ms tick in 65 s
		w.add(7, 3, 0xFFFF0100u);
		w.run(400000);
		check(w.s.station(7).synced && w.s.station(7).missed == 0 && w.heard[7] > 80, "across the 32 bit ms rollover: synced, nothing missed");
	}
	for (uint32_t rate : {32768u, 2048u, 256u}) {
		// The RTC's rates: the 32.768 kHz crystal, and its subsecond counter
		// at 0.49 ms and at CubeMX's default 3.9 ms.
		World w;
		w.start(500, 0x05, davis_band_us, rate);
		w.add(0, 7, 900);
		w.add(2, 33, 1700);
		w.run(300000);
		const uint32_t h0 = w.heard[0], s0 = w.tx[0].sent;
		w.run(300000);
		char what[96];
		std::snprintf(what, sizeof what, "RTC at %u ticks/s: two stations synced, station 1 none missed (%u/%u)",
			rate, w.heard[0] - h0, w.tx[0].sent - s0);
		check(w.s.station(0).synced && w.s.station(2).synced && w.heard[0] - h0 + 1 >= w.tx[0].sent - s0, what);
		check(intervalTicks(0, rate) * 16 == 41 * rate, "and the interval a whole number of its ticks");
	}
	{
		// The clock set while running, with nobody telling the schedule:
		// it notices the jump and starts over.
		World w;
		w.start(0, 0x01, davis_band_us, 32768);
		w.add(0, 4, 300);
		w.run(140000);
		w.clockOffset = -3600LL * 32768;			// an hour back
		w.run(10);
		check(w.s.clockJumps() == 1 && !w.s.station(0).synced, "clock set an hour back unannounced: noticed, starts over");
		w.run(140000);
		check(w.s.station(0).synced, "and finds the station again");
		// Told about it (shift()), nothing is lost.
		const uint32_t missed = w.s.station(0).missed;
		w.clockOffset += 3600LL * 32768;
		w.s.shift((int32_t)(3600LL * 32768));
		w.run(60000);
		check(w.s.clockJumps() == 1 && w.s.station(0).synced && w.s.station(0).missed == missed,
			"set an hour on and shift()ed: nothing missed");
	}
	{
		World w;
		w.start(0, 0x01, davis_band_eu);
		w.add(0, 2, 700);
		w.run(30000);
		check(w.s.station(0).synced && w.s.channels() == 5, "EU band, 5 channels: found in seconds");
	}
	{
		World w;
		w.start(0, 0x01);
		w.add(0, 0, 100);
		w.run(140000);
		const uint32_t heard = w.heard[0];
		// After 500 packets the expected time must still be exact: a
		// rounded interval would drift 0.5 ms a packet.
		w.run(500 * 2563);
		check(w.heard[0] - heard >= 499 && w.s.station(0).missed == 0, "500 packets later, still exact (1/16 ms timing)");
		w.s.resync(w.now);
		check(!w.s.station(0).synced, "resync forgets it");
		w.run(140000);
		check(w.s.station(0).synced, "and finds it again");
	}
}

static void weather() {
	std::printf("DavisWeather\n");
	DavisWeather w(false);
	check(std::isnan(w.temperatureF) && w.rainTips == 0, "NAN and no rain at first");
	DavisPacket p = {};
	uint8_t d[6] = {0x80, 7, 128, 0x2D, 0x50, 0};
	buildPacket(d, p.raw);
	p.rxMs = 1234;
	w.update(p);
	check(near(w.temperatureF, 72.5f, 0.01f) && w.windSpeedMph == 7 && w.lastUpdateMs == 1234, "temperature and wind from a packet");
	auto rain = [&](uint8_t counter) {
		uint8_t r[6] = {0xE0, 0, 0, counter, 0, 0};
		buildPacket(r, p.raw);
		w.update(p);
	};
	rain(120);
	check(w.rainTips == 0, "the first counter is the start");
	rain(125);
	rain(2);									// 125 -> 127 -> 0 -> 2: 5 tips
	check(w.rainTips == 10 && near(w.rainInches(), 0.10f, 0.001f), "5 + 5 tips across the wrap at 128: 0.10 in");
	rain(0x80);
	check(w.rainTips == 10, "no gauge (0x80): nothing added");
	uint8_t rs[6] = {0x50, 0, 0, 0x2C, 0x41, 0};
	buildPacket(rs, p.raw);
	w.update(p);
	check(near(w.rainRateInHr, 36.0f / 44.0f, 0.001f), "rate from 44 s between tips");
	uint8_t dry[6] = {0x50, 0, 0, 0xFF, 0x30, 0};
	buildPacket(dry, p.raw);
	w.update(p);
	check(w.rainRateInHr == 0.0f, "no tips lately: rate 0");
}

int main() {
	protocol();
	schedule();
	weather();
	std::printf("%s (%d failed)\n", g_failures ? "FAILED" : "all passed", g_failures);
	return g_failures ? 1 : 0;
}
