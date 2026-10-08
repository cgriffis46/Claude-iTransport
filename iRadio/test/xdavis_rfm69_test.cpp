/*
 * xdavis_rfm69_test.cpp
 *
 * Host test for xdavis_rfm69: the receiver as a FreeRTOS task, with its
 * packet queue, log stream buffer, command queue and DIO0 wake-up, over
 * a single threaded FreeRTOS stand-in (test/stub) in which time moves
 * only while the task sleeps. The radio and the stations are the
 * simulations of test/sim/SimDavis.h. IT is the itransport folder:
 *
 *   g++ -std=c++17 -Wall -Wextra -Istub -Isim -I../hw/freertos/inc -I../davis/inc -I../rfm69/inc \
 *       -I$IT/inc -I<isensor>/inc xdavis_rfm69_test.cpp ../davis/src/DavisProtocol.cpp \
 *       ../davis/src/DavisSchedule.cpp ../davis/src/DavisWeather.cpp $IT/src/BusTransport.cpp \
 *       $IT/src/SPITransport.cpp -o xdavis_rfm69_test
 */

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include "SimDavis.h"
#include "xdavis_rfm69.h"
#include "DavisWeather.h"

using namespace DAVIS;

static int g_failures = 0;
static void check(bool ok, const char* what) {
	std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok) ++g_failures;
}

// The radio task and the world around it: the stations send into the
// simulated chip as time passes, and a consumer task takes packets off
// the queue and lines out of the stream buffer (unless told not to).
struct Rig {
	sim::Rfm69 chip;
	sim::Iss iss;
	xdavis_rfm69<sim::Bus> radio;
	bool consume = true;
	std::vector<DavisPacket> got;
	std::string log;
	std::vector<uint32_t> endTimes;		// when each packet the chip took ended

	Rig(const davis_param_t& p, uint8_t issId = 0)
		: iss(issId, p.band, 21, 900), radio(p, chip) {
		stubrtos::tick() = 0;
		stubrtos::task() = stubrtos::Task{nullptr, nullptr, 0, nullptr, 0, 0};
		stubrtos::onTick() = [this](TickType_t t) { world(t); };
		if (p.dio0_interrupt) chip.dio0 = [this] { endTimes.push_back(chip.now); radio.onDio0FromISR(); };
		else chip.dio0 = [this] { endTimes.push_back(chip.now); };
	}
	void world(uint32_t t) {
		chip.now = t;
		iss.tick(t, chip);
		if (!consume) return;
		DavisPacket p;
		while (radio.packets() && xQueueReceive(radio.packets(), &p, 0) == pdPASS) got.push_back(p);
		char buf[64];
		size_t n;
		while (radio.log() && (n = xStreamBufferReceive(radio.log(), buf, sizeof buf, 0)) > 0) log.append(buf, n);
	}
	// The radio task's loop, until the clock reaches ms more.
	void run(uint32_t ms) {
		const uint32_t end = stubrtos::tick() + ms;
		while ((int32_t)(stubrtos::tick() - end) < 0) radio.step();
	}
};

static void task() {
	std::printf("xdavis_rfm69: start, queue, stream buffer\n");
	davis_param_t p = davis_default_param();
	Rig r(p);
	xdavis_config_t cfg;
	cfg.logBytes = 256;
	check(r.radio.start("davis", 512, tskIDLE_PRIORITY + 3, cfg), "start");
	check(stubrtos::created() >= 1 && stubrtos::task().stack == 512 && stubrtos::task().prio == tskIDLE_PRIORITY + 3,
		"the task created with its stack and priority");
	check(r.radio.packets() && r.radio.packets()->depth == 8 && r.radio.packets()->item == sizeof(DavisPacket),
		"a queue of 8 DavisPackets");
	check(r.radio.log() && r.radio.log()->size == 256, "a 256 byte stream buffer for the log");

	r.run(140000);
	check(r.radio.schedule().station(0).synced, "found the station");
	const size_t first = r.got.size();
	const uint32_t sent = r.iss.sent;
	r.run(300000);
	const uint32_t n = (uint32_t)(r.got.size() - first);
	std::printf("        5 min: %u of %u packets through the queue, longest sleep %u ms\n", n, r.iss.sent - sent, stubrtos::longestSleep());
	check(n == r.iss.sent - sent, "every packet through the queue");
	check(stubrtos::longestSleep() <= 2, "polled: sleeps 2 ms at a time between reads of the flags");

	// The log: one line per packet, as documented.
	size_t lines = 0;
	for (char c : r.log) if (c == '\n') ++lines;
	check(lines == r.got.size(), "one log line per packet");
	const std::string last = r.log.substr(r.log.rfind('\n', r.log.size() - 2) + 1);
	const DavisPacket& lp = r.got.back();
	char want[96];
	int k = std::snprintf(want, sizeof want, "%lu 1 %u -70 %ld", (unsigned long)lp.rxMs, (unsigned)lp.channel, (long)lp.feiHz);
	for (int i = 0; i < kPacketLen; ++i) k += std::snprintf(want + k, sizeof want - (size_t)k, " %02X", lp.raw[i]);
	want[k++] = '\n'; want[k] = 0;
	std::printf("        %s", last.c_str());
	check(last == want, "<ms> <transmitter> <channel> <rssi> <fei> <10 bytes>");

	DavisWeather wx(false);
	for (const DavisPacket& q : r.got) wx.update(q);
	check(std::fabs(wx.temperatureF - 72.5f) < 0.01f, "the consumer decodes 72.5 F");

	// A consumer that stops: the queue fills (8), the rest are dropped and
	// counted; the log keeps whole lines only.
	r.consume = false;
	const uint32_t dropped = r.radio.stats().dropped;
	r.run(12 * 2563);
	check(r.radio.packets()->q.size() == 8 && r.radio.stats().dropped >= dropped + 3, "queue full: packets dropped and counted");
	check(r.radio.logDropped() > 0, "stream buffer full: lines left out and counted");
	bool whole = r.radio.log()->b.empty() || r.radio.log()->b.back() == '\n';
	check(whole, "and never half a line");
	r.consume = true;
	r.run(10);
	check(r.radio.schedule().station(0).synced && r.radio.schedule().station(0).missed == 0, "the radio carried on: nothing missed");
}

static void commands() {
	std::printf("xdavis_rfm69: commands\n");
	davis_param_t p = davis_default_param();
	p.active_stations = 0x00;				// nothing to start with
	Rig r(p, 3);
	r.radio.start();
	r.run(60000);
	check(r.got.empty() && !r.radio.schedule().station(3).active, "no station active: nothing");
	stubrtos::task().notify = 0;
	check(r.radio.postStationActive(3, true), "postStationActive(3) from another task");
	check(stubrtos::task().notify > 0, "and the radio task is woken");
	r.run(160000);
	check(r.radio.schedule().station(3).synced && !r.got.empty() && r.got.back().station == 3, "station 3 (transmitter 4) found");
	check(r.radio.postResync(), "postResync");
	r.run(5);
	check(!r.radio.schedule().station(3).synced && r.radio.schedule().station(3).active, "applied: station 3 forgotten, still listened for");
	check(r.radio.postBand(davis_band_eu), "postBand(EU)");
	r.run(5);
	check(r.radio.schedule().band() == davis_band_eu && r.radio.schedule().channels() == 5, "band changed, 5 channels");
	for (int i = 0; i < 4; ++i) r.radio.postStationActive(1, true);
	check(!r.radio.postStationActive(1, true), "the command queue (4) full: refused");
}

static void dio0() {
	std::printf("xdavis_rfm69: DIO0 interrupt\n");
	davis_param_t p = davis_default_param();
	p.dio0_interrupt = true;
	Rig r(p);
	r.radio.start();
	r.run(140000);
	const int reads = r.chip.flagReads;
	const size_t first = r.got.size();
	stubrtos::longestSleep() = 0;
	r.run(300000);
	std::printf("        5 min: %u packets, %d flag reads, longest sleep %u ms\n",
		(unsigned)(r.got.size() - first), r.chip.flagReads - reads, stubrtos::longestSleep());
	check(r.got.size() - first >= 115 && r.radio.schedule().station(0).missed == 0, "every packet");
	check(r.chip.flagReads - reads < 4000, "flags read on the edge and every 100 ms, not every 2");
	check(stubrtos::longestSleep() >= 90, "the task sleeps for long stretches");
	bool exact = true;
	for (size_t i = first; i < r.got.size(); ++i) {
		bool found = false;
		for (uint32_t e : r.endTimes) if (e == r.got[i].rxMs) found = true;
		if (!found) exact = false;
	}
	check(exact, "the interrupt cut each sleep short: rx time is the very ms the packet ended");
}

static void rtcTimestamp() {
	std::printf("xdavis_rfm69: RTC clock, DIO0 latched by the RTC's timestamp unit\n");
	struct Clock : public iClock {
		uint32_t ticksPerSecond() const override { return 32768; }
		uint32_t now() override { return (uint32_t)((uint64_t)stubrtos::tick() * 32768u / 1000u); }
	} rtc;
	davis_param_t p = davis_default_param();
	p.dio0_interrupt = true;
	Rig r(p);
	r.radio.setClock(&rtc);
	// The callback of the timestamp event: the latched time is the
	// packet's end, exact; the interrupt runs whenever it gets to run.
	r.chip.dio0 = [&r] { r.radio.onDio0FromISRAt((uint32_t)((uint64_t)r.iss.next16 * 32768u / 16000u)); };
	r.radio.start();
	r.run(140000);
	const size_t first = r.got.size();
	r.run(300000);
	bool exact = r.got.size() > first + 100;
	for (size_t i = first + 1; i < r.got.size(); ++i)
		if (r.got[i].rxTicks - r.got[i - 1].rxTicks != 41u * 2048u) exact = false;
	check(exact && r.radio.schedule().station(0).missed == 0, "every packet, 83968 RTC ticks apart exactly");
}

int main() {
	task();
	rtcTimestamp();
	commands();
	dio0();
	std::printf("%s (%d failed)\n", g_failures ? "FAILED" : "all passed", g_failures);
	return g_failures ? 1 : 0;
}
