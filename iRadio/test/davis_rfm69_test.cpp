/*
 * davis_rfm69_test.cpp
 *
 * Host test for davis_rfm69<TTransport>: the driver against a simulated
 * RFM69 (test/sim/SimDavis.h) and simulated ISS stations on the real
 * timing. No hardware, HAL or RTOS needed. IT is the itransport folder:
 *
 *   g++ -std=c++17 -Wall -Wextra -I../davis/inc -I../rfm69/inc -Isim -I$IT/inc -I<isensor>/inc \
 *       davis_rfm69_test.cpp ../davis/src/DavisProtocol.cpp ../davis/src/DavisSchedule.cpp \
 *       ../davis/src/DavisWeather.cpp $IT/src/BusTransport.cpp $IT/src/SPITransport.cpp -o davis_rfm69_test
 */

#include <cmath>
#include <cstdio>
#include <vector>
#include "SimDavis.h"
#include "davis_rfm69.h"
#include "DavisWeather.h"

using namespace DAVIS;

static int g_failures = 0;
static void check(bool ok, const char* what) {
	std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok) ++g_failures;
}

// The world: one radio, some stations, a clock. main() runs twice a ms,
// as a task would that is woken by its own sleeps.
template <typename TRadio>
struct World {
	sim::Rfm69& chip;
	TRadio& radio;
	std::vector<sim::Iss*> stations;
	uint32_t now;
	std::vector<DavisPacket> got;
	bool drain = true;

	World(sim::Rfm69& c, TRadio& r, uint32_t start) : chip(c), radio(r), now(start) {}

	void run(uint32_t ms) {
		for (uint32_t i = 0; i < ms; ++i, ++now) {
			chip.now = now;
			for (sim::Iss* s : stations) s->tick(now, chip);
			radio.main(now);
			radio.main(now);
			DavisPacket p;
			while (drain && radio.readPacket(p)) got.push_back(p);
		}
	}
	uint32_t runUntilSynced(uint8_t id, uint32_t limitMs) {
		const uint32_t start = now;
		while (!radio.schedule().station(id).synced && now - start < limitMs) run(100);
		return now - start;
	}
};

static void configuration() {
	std::printf("davis_rfm69<sim::Bus>: start-up\n");
	sim::Rfm69 chip;
	davis_rfm69<sim::Bus> radio(davis_default_param(), chip);
	World<davis_rfm69<sim::Bus>> w(chip, radio, 0);
	w.run(50);
	check(radio.ready(), "version checked, configured, sync word read back");
	check(chip.davisConfigured(), "the Davis settings: FSK 19.2 kb/s, 9.8 kHz, sync CB 89, 10 byte fixed packets");
	check(chip.reg[RFM69_REG_RXBW] == 0x4C && chip.reg[RFM69_REG_AFCBW] == 0x4B, "25 kHz receive bandwidth, 50 kHz for AFC");
	check(chip.reg[RFM69_REG_RSSITHRESH] == 190, "RSSI threshold -95 dBm (190)");
	check(chip.reg[RFM69_REG_PACKETCONFIG2] == 0x12 && chip.reg[RFM69_REG_TESTDAGC] == 0x30, "AutoRxRestart, improved DAGC");
	check(chip.inRx(), "receiving");
	const uint8_t* f = bandFrf(davis_band_us, radio.schedule().discoveryChannel());
	check(chip.reg[7] == f[0] && chip.reg[8] == f[1] && chip.reg[9] == f[2], "on the discovery channel's frequency");

	davis_param_t wide = davis_default_param();
	wide.wide_bandwidth = true;
	wide.rssi_threshold_dbm = -100;
	sim::Rfm69 chip2;
	davis_rfm69<sim::Bus> radio2(wide, chip2);
	World<davis_rfm69<sim::Bus>> w2(chip2, radio2, 0);
	w2.run(50);
	check(chip2.reg[RFM69_REG_RXBW] == 0x4B && chip2.reg[RFM69_REG_RSSITHRESH] == 200, "wide: 50 kHz; threshold -100 dBm (200)");
}

template <typename TTransport>
static void receive(const char* name, bool dio0) {
	std::printf("%s%s: receiving one ISS\n", name, dio0 ? ", DIO0 interrupt" : ", polled");
	sim::Rfm69 chip;
	davis_param_t param = davis_default_param();
	param.dio0_interrupt = dio0;
	davis_rfm69<TTransport> radio(param, chip);
	if (dio0) chip.dio0 = [&] { radio.onDio0(chip.now); };
	sim::Iss iss(0, davis_band_us, 37, 900);
	World<davis_rfm69<TTransport>> w(chip, radio, 0);
	w.stations.push_back(&iss);

	const uint32_t took = w.runUntilSynced(0, 300000);
	std::printf("        found after %.1f s\n", took / 1000.0);
	check(radio.schedule().station(0).synced && took <= 51 * 2563 + 3000, "found within one hop cycle");
	const size_t first = w.got.size();
	const uint32_t sent = iss.sent;
	const int reads = chip.flagReads;
	w.run(600000);
	const uint32_t n = (uint32_t)(w.got.size() - first);
	std::printf("        10 min: %u of %u packets, %d flag reads\n", n, iss.sent - sent, chip.flagReads - reads);
	check(n == iss.sent - sent && radio.schedule().station(0).missed == 0, "10 minutes: every packet, none missed");
	if (dio0) check(chip.flagReads - reads < 10000, "DIO0: RegIrqFlags2 read only on the edge and every 100 ms");
	else check(chip.flagReads - reads > 100000, "polled: RegIrqFlags2 read every 2 ms");

	if (w.got.empty()) {
		check(false, "packets received at all");
		return;
	}
	const DavisPacket& p = w.got.back();
	check(p.station == 0 && p.rssi == -70 && p.feiHz == -20 * 15625 / 256 && !p.viaRepeater, "station 0, -70 dBm, FEI -1220 Hz");
	check(checkCrc(p.raw) == davis_crc_direct, "bits back in order, CRC good");
	bool channelsRight = true;
	for (size_t i = first + 1; i < w.got.size(); ++i)
		if (w.got[i].channel != (w.got[i - 1].channel + 1) % 51) channelsRight = false;
	check(channelsRight, "each packet one channel on from the last");
	bool timingRight = true;
	for (size_t i = first + 1; i < w.got.size(); ++i) {
		const uint32_t gap = w.got[i].rxMs - w.got[i - 1].rxMs;
		if (gap < 2560 || gap > 2565) timingRight = false;
	}
	check(timingRight, "2562.5 ms apart");
	DavisWeather wx(false);
	for (size_t i = first; i < w.got.size(); ++i) wx.update(w.got[i]);
	check(std::fabs(wx.temperatureF - 72.5f) < 0.01f && std::fabs(wx.humidity - 45.6f) < 0.01f && wx.windSpeedMph == 7,
		"decoded: 72.5 F, 45.6 %, 7 mph");
	check(wx.rainTips > 0 && wx.windGustMph == 15, "rain counted across the wrap, gust 15 mph");
}

static void faults() {
	std::printf("davis_rfm69<sim::Bus>: bad packets, outages, stations\n");
	{
		sim::Rfm69 chip;
		davis_rfm69<sim::Bus> radio(davis_default_param(), chip);
		sim::Iss iss(0, davis_band_us, 3, 500);
		World<davis_rfm69<sim::Bus>> w(chip, radio, 0);
		w.stations.push_back(&iss);
		w.runUntilSynced(0, 300000);
		w.run(10000);
		iss.corruptNext = true;
		const size_t before = w.got.size();
		for (int i = 0; i < 3000 && radio.stats().crc_errors == 0; ++i) w.run(1);
		w.run(5);
		check(radio.stats().crc_errors == 1 && w.got.size() == before, "a corrupted packet: counted, not delivered");
		w.run(30);								// past the 20 ms late window
		check(radio.schedule().station(0).missed == 1 && radio.schedule().station(0).synced, "its slot missed, still synced");
		w.run(10000);
		check(w.got.size() >= before + 3, "the next ones come in");

		iss.on = false;
		w.run(40000);
		iss.on = true;
		const size_t back = w.got.size();
		w.run(5000);
		check(w.got.size() > back && radio.schedule().station(0).synced, "silent 40 s: picked up again where it hopped to");
		iss.on = false;
		w.run(160000);
		check(!radio.schedule().station(0).synced && radio.schedule().station(0).resyncs == 1, "silent 160 s: lost");
		iss.on = true;
		w.runUntilSynced(0, 300000);
		check(radio.schedule().station(0).synced, "and found again");
	}
	{
		sim::Rfm69 chip;
		davis_param_t param = davis_default_param();
		davis_rfm69<sim::Bus> radio(param, chip);
		sim::Iss a(0, davis_band_us, 10, 400), b(2, davis_band_us, 30, 1100);
		World<davis_rfm69<sim::Bus>> w(chip, radio, 0);
		w.stations.push_back(&a);
		w.stations.push_back(&b);
		w.run(200000);
		bool only0 = true;
		for (const DavisPacket& p : w.got) if (p.station != 0) only0 = false;
		check(only0 && radio.schedule().station(0).synced, "station 2 not active: only station 0 delivered");
		radio.setStationActive(2, true);
		w.runUntilSynced(2, 300000);
		check(radio.schedule().station(2).synced && radio.schedule().station(0).synced, "made active: both synced");
		const uint32_t s0 = a.sent, s2 = b.sent;
		w.got.clear();
		w.run(300000);
		uint32_t g0 = 0, g2 = 0;
		for (const DavisPacket& p : w.got) (p.station == 0 ? g0 : g2)++;
		std::printf("        5 min: station 1 %u/%u, station 3 %u/%u\n", g0, a.sent - s0, g2, b.sent - s2);
		check(g0 * 100 >= (a.sent - s0) * 95 && g2 * 100 >= (b.sent - s2) * 95, "two stations: 95 % of each (clashes cost the rest)");
	}
	{
		// Station 4 is switched on but not there: the receiver looks for it
		// between station 0's packets for ever, and must be back on station
		// 0's channel, settled, before each of its packets starts (6.7 ms
		// before it ends).
		sim::Rfm69 chip;
		davis_param_t param = davis_default_param();
		param.active_stations = 0x11;
		davis_rfm69<sim::Bus> radio(param, chip);
		sim::Iss iss(0, davis_band_us, 3, 500);
		World<davis_rfm69<sim::Bus>> w(chip, radio, 0);
		w.stations.push_back(&iss);
		w.runUntilSynced(0, 300000);
		const uint32_t sent = iss.sent;
		w.got.clear();
		w.run(300000);
		check(radio.schedule().discoveryStation() == 4 && !radio.schedule().station(4).synced, "an absent station: looked for all the time");
		check(w.got.size() == iss.sent - sent && radio.schedule().station(0).missed == 0, "and the present one loses nothing to it");
	}
	{
		sim::Rfm69 chip;
		davis_rfm69<sim::Bus> radio(davis_default_param(), chip);
		sim::Iss iss(0, davis_band_us, 3, 500);
		World<davis_rfm69<sim::Bus>> w(chip, radio, 0);
		w.stations.push_back(&iss);
		w.runUntilSynced(0, 300000);
		w.drain = false;
		w.run(6 * 2563);
		check(radio.stats().dropped >= 2, "nobody reading: the ring keeps 4, the rest dropped and counted");
		w.drain = true;
		w.run(10);
		check(w.got.size() >= 4, "and the 4 come out when asked");
	}
	{
		sim::Rfm69 chip;
		chip.present = false;
		davis_rfm69<sim::Bus> radio(davis_default_param(), chip);
		World<davis_rfm69<sim::Bus>> w(chip, radio, 0);
		w.run(50);
		check(!radio.ready() && radio.state() == davis_error, "no radio: the version read fails, error");
		chip.present = true;
		w.run(1200);
		check(radio.ready() && chip.davisConfigured(), "radio back: configured by itself after the backoff");
	}
	{
		sim::Rfm69 chip;
		chip.version = 0x00;
		davis_rfm69<sim::Bus> radio(davis_default_param(), chip);
		World<davis_rfm69<sim::Bus>> w(chip, radio, 0);
		w.run(50);
		check(radio.state() == davis_error && chip.writes == 0, "version 0x00 (not an RFM69): error, nothing written");
	}
	{
		sim::Rfm69 chip;
		davis_rfm69<sim::Bus> radio(davis_default_param(), chip);
		World<davis_rfm69<sim::Bus>> w(chip, radio, 0);
		w.run(50);
		chip.stuck = true;
		w.run(200);
		check(radio.state() == davis_error, "a transfer that never lands: error at the 100 ms bus timeout");
		chip.stuck = false;
		w.run(1200);
		check(radio.ready() && chip.inRx(), "and back to receiving");
	}
	{
		sim::Rfm69 chip;
		davis_rfm69<sim::Bus> radio(davis_default_param(), chip);
		sim::Iss iss(5, davis_band_eu, 1, 300);
		radio.setBand(davis_band_eu);
		radio.setStationActive(0, false);
		radio.setStationActive(5, true);
		World<davis_rfm69<sim::Bus>> w(chip, radio, 0xFFFF8000u);	// the ms tick rolls over in 33 s
		w.stations.push_back(&iss);
		w.runUntilSynced(5, 60000);
		w.got.clear();
		w.run(120000);
		check(radio.schedule().band() == davis_band_eu && radio.schedule().station(5).synced, "EU band, station 5 (transmitter 6)");
		check(radio.schedule().station(5).missed == 0 && w.got.size() >= 40, "across the tick rollover, nothing missed");
	}
}

// ---- timing by a clock other than the RTOS tick ----

// A clock running on true time (the RTC's crystal), set away from it by
// offset when the test sets it.
struct FakeClock : public iClock {
	uint32_t tps;
	uint32_t trueMs = 0;
	int64_t offset = 0;
	explicit FakeClock(uint32_t ticksPerSecond) : tps(ticksPerSecond) {}
	uint32_t ticksPerSecond() const override { return tps; }
	uint32_t now() override { return (uint32_t)((int64_t)((uint64_t)trueMs * tps / 1000u) + offset); }
	// A time in sixteenths of a true ms, in this clock's ticks: what the
	// RTC's timestamp unit would latch.
	uint32_t at16(uint32_t t16) const { return (uint32_t)((int64_t)((uint64_t)t16 * tps / 16000u) + offset); }
};

// True time moves in ms; the CPU's tick (and so nowMs) can run fast or
// slow of it, as an uncalibrated RC oscillator does.
struct ClockWorld {
	sim::Rfm69 chip;
	davis_rfm69<sim::Bus> radio;
	FakeClock clock;
	sim::Iss iss;
	uint32_t trueMs = 0;
	int32_t tickPpm = 0;			// how fast the RTOS tick runs, parts per million
	std::vector<DavisPacket> got;

	ClockWorld(const davis_param_t& p, uint32_t tps, int32_t ppm)
		: radio(p, chip), clock(tps), iss(0, davis_band_us, 12, 1000), tickPpm(ppm) {}

	uint32_t tickMs() const { return (uint32_t)((int64_t)trueMs * (1000000 + tickPpm) / 1000000); }

	void run(uint32_t ms) {
		for (uint32_t i = 0; i < ms; ++i, ++trueMs) {
			chip.now = trueMs;
			clock.trueMs = trueMs;
			iss.tick(trueMs, chip);
			radio.main(tickMs());
			radio.main(tickMs());
			DavisPacket p;
			while (radio.readPacket(p)) got.push_back(p);
		}
	}
	// Packets received out of those sent over the next ms.
	uint32_t share(uint32_t ms) {
		const size_t g = got.size();
		const uint32_t s = iss.sent;
		run(ms);
		return (uint32_t)((got.size() - g) * 100 / (iss.sent - s));
	}
};

static void clocks() {
	std::printf("davis_rfm69 timed by a clock (the STM32 RTC)\n");
	davis_param_t p = davis_default_param();
	{
		// The CPU on an RC oscillator 1 % fast: by the RTOS tick, each
		// packet comes 26 ms later than it should, past the 20 ms window.
		ClockWorld tick(p, 16000, 10000);
		tick.run(150000);
		const uint32_t byTick = tick.share(600000);
		ClockWorld rtc(p, 32768, 10000);
		rtc.radio.setClock(&rtc.clock);
		rtc.run(150000);
		const uint32_t byRtc = rtc.share(600000);
		std::printf("        CPU clock 1 %% fast, 10 min: %u %% of packets by the tick, %u %% by the RTC\n", byTick, byRtc);
		check(byTick < 50, "timed by the RTOS tick: most packets lost");
		check(byRtc == 100 && rtc.radio.schedule().station(0).missed == 0, "timed by the RTC's crystal: every packet");
		check(rtc.radio.ticksPerSecond() == 32768 && rtc.radio.schedule().ticksPerSecond() == 32768, "the schedule counts in the RTC's ticks");
	}
	{
		// DIO0 on the RTC_TS pin: the RTC latches the end of each packet in
		// hardware, to the tick.
		davis_param_t q = p;
		q.dio0_interrupt = true;
		ClockWorld w(q, 32768, 0);
		w.radio.setClock(&w.clock);
		w.chip.dio0 = [&w] { w.radio.onDio0At(w.clock.at16(w.iss.next16)); };
		w.run(150000);
		const size_t first = w.got.size();
		w.run(300000);
		bool exact = w.got.size() > first + 100;
		for (size_t i = first + 1; i < w.got.size(); ++i)
			if (w.got[i].rxTicks - w.got[i - 1].rxTicks != 41u * 2048u) exact = false;
		check(exact, "hardware timestamps: every packet exactly 83968 ticks (2.5625 s) after the last");
	}
	{
		// DIO0 on an ordinary EXTI pin: the interrupt reads the clock.
		davis_param_t q = p;
		q.dio0_interrupt = true;
		ClockWorld w(q, 32768, 0);
		w.radio.setClock(&w.clock);
		w.chip.dio0 = [&w] { w.radio.onDio0(w.tickMs()); };
		w.run(150000);
		const size_t first = w.got.size();
		w.run(300000);
		bool close = w.got.size() > first + 100;
		for (size_t i = first + 1; i < w.got.size(); ++i) {
			const int32_t gap = (int32_t)(w.got[i].rxTicks - w.got[i - 1].rxTicks) - 41 * 2048;
			if (gap < -33 || gap > 33) close = false;
		}
		check(close, "stamped from the clock in the interrupt: within a ms (33 ticks)");
	}
	{
		// The RTC set an hour on (from SNTP) while receiving: seen as the
		// clock jumping against the RTOS tick, and the schedule moved.
		ClockWorld w(p, 2048, 0);
		w.radio.setClock(&w.clock);
		w.run(150000);
		w.run(60000);
		const uint32_t missed = w.radio.schedule().station(0).missed;
		w.clock.offset += 3600LL * 2048;
		w.run(60000);
		w.clock.offset -= 7200LL * 2048;				// and two hours back
		const uint32_t share = w.share(60000);
		check(w.radio.stats().clock_steps == 2 && w.radio.schedule().clockJumps() == 0, "RTC set on an hour, then back two: both steps seen");
		check(w.radio.schedule().station(0).missed == missed && share == 100, "and not a packet missed");
	}
	{
		// CubeMX's default prescalers: 3.9 ms ticks.
		ClockWorld w(p, 256, 0);
		w.radio.setClock(&w.clock);
		w.run(150000);
		check(w.share(300000) == 100 && w.radio.schedule().station(0).missed == 0, "RTC at 256 ticks/s (CubeMX default): every packet");
	}
	{
		// Given the clock while running: the schedule starts over in its ticks.
		ClockWorld w(p, 32768, 0);
		w.run(150000);
		check(w.radio.schedule().station(0).synced && w.radio.schedule().ticksPerSecond() == 16000, "running on the tick");
		w.radio.setClock(&w.clock);
		w.run(1000);
		check(w.radio.schedule().ticksPerSecond() == 32768 && !w.radio.schedule().station(0).synced, "setClock(): starts over on the RTC");
		w.run(150000);
		check(w.radio.schedule().station(0).synced, "and finds the station again");
	}
}

int main() {
	configuration();
	receive<sim::Bus>("davis_rfm69<sim::Bus>", false);
	receive<sim::Bus>("davis_rfm69<sim::Bus>", true);
	receive<sim::Spi>("davis_rfm69<sim::Spi> (the real SPITransport)", false);
	faults();
	clocks();
	std::printf("%s (%d failed)\n", g_failures ? "FAILED" : "all passed", g_failures);
	return g_failures ? 1 : 0;
}
