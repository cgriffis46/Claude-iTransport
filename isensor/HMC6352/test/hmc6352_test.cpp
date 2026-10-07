/*
 * hmc6352_test.cpp
 *
 * Host test for hmc6352<TTransport>. No hardware, HAL or RTOS needed:
 *
 *   g++ -std=c++17 -Wall -Wextra -I../inc -Istub \
 *       -I<safeTransport>/itransport/inc -I<safeTransport>/isensor/inc \
 *       hmc6352_test.cpp <safeTransport>/itransport/src/BusTransport.cpp \
 *       <safeTransport>/itransport/src/I2CTransport.cpp -o hmc6352_test
 *
 * The simulated chip below was written from the same reading of the
 * datasheet as the driver. It shows the driver does what was intended;
 * it cannot show that the intention matches the real chip.
 */

#include <cstdio>
#include <cmath>
#include <vector>
#include "I2CTransport.h"
#include "xhmc6352.h"	// pulls in hmc6352.h. stub/cmsis_os2.h stands in for the RTOS

using namespace HMC6352;

uint32_t g_slept = 0;						// set by the stub osDelay()
std::vector<uint32_t> g_delays;

// ---- simulated HMC6352: takes letter commands, is read back as two bytes ----
struct MockChip {
	uint32_t now = 0;						// the test's clock
	bool present = true;
	bool stuck = false;						// operations never complete
	bool refuse = false;					// transport cannot start an operation
	int16_t value = 1234;					// what the next measurement will give: 123.4 degrees

	uint8_t opMode = 0x50;					// RAM 0x74. power-on: standby, periodic set/reset on, 20 Hz
	uint8_t outputMode = 0x00;				// RAM 0x4E
	int16_t latched = 0;					// what the output register holds
	bool converting = false; uint32_t convEnd = 0;
	int getDatas = 0, reads = 0, earlyReads = 0, ramWrites = 0;
	std::vector<std::vector<uint8_t>> commands;

	bool continuous() const { return (opMode & 0x03) == 0x02; }
	bool write(const uint8_t* b, uint16_t n) {			// false: not acknowledged
		if (!present || n < 1) return false;
		commands.emplace_back(b, b + n);
		if (b[0] == 0x47 && n == 3) {					// 'G': write RAM
			++ramWrites;
			if (b[1] == 0x74) opMode = b[2];
			if (b[1] == 0x4E) outputMode = b[2];
		} else if (b[0] == 0x41 && n == 1) {			// 'A': get data
			++getDatas; converting = true; convEnd = now + 6;
		}
		return true;
	}
	bool read(uint8_t* out, uint16_t n) {
		if (!present || n != 2) return false;
		++reads;
		if (continuous()) {
			latched = value;							// always fresh in continuous mode
		} else if (converting) {
			if ((int32_t)(now - convEnd) < 0) ++earlyReads;	// read before the 6 ms are up: old data
			else { latched = value; converting = false; }
		}
		out[0] = (uint8_t)((uint16_t)latched >> 8); out[1] = (uint8_t)((uint16_t)latched & 0xFF);
		return true;
	}
};

// ---- transport 1: a bare ISensorTransport whose operations take time ----
class MockBus : public ISensorTransport {
public:
	explicit MockBus(MockChip& c) : chip(c) {}
	bool writeReg(uint8_t, uint8_t) override { return false; }		// a command-style chip uses none of these three
	bool writeRegs(uint8_t, const uint8_t*, uint8_t) override { return false; }
	bool readRegs(uint8_t, uint8_t*, uint8_t) override { return false; }
	bool writeBytes(const uint8_t* buf, uint8_t len) override {
		if (busy || chip.refuse) return false;
		failed = !chip.write(buf, len);
		busy = true; polls = 1; dest = nullptr;
		return true;
	}
	bool readBytes(uint8_t* buf, uint8_t len) override {
		if (busy || chip.refuse) return false;
		failed = false;
		busy = true; polls = 1; dest = buf; n = len;
		return true;
	}
	bool isBusy() const override {
		if (!busy) return false;
		if (chip.stuck) return true;
		if (polls > 0) { --polls; return true; }
		if (dest && !chip.read(dest, n)) failed = true;				// lands only now
		busy = false;
		return false;
	}
	bool lastOpFailed() const override { return failed; }
	bool checkDevice() override { return chip.present; }
private:
	MockChip& chip;
	mutable bool busy = false; mutable int polls = 0; mutable bool failed = false;
	uint8_t* dest = nullptr; uint8_t n = 0;
};

// ---- transport 2: itransport's real I2CTransport, with the HAL calls simulated ----
static int g_i2cBus;	// stands in for &hi2c1
class LoopI2C : public I2CTransport {
public:
	explicit LoopI2C(MockChip& c) : I2CTransport(&g_i2cBus, HMC6352_I2C_ADDR, nullptr), chip(c) {}
protected:
	bool halMemWrite(uint8_t, uint8_t*, uint16_t) override { return false; }
	bool halMemRead(uint8_t, uint8_t*, uint16_t) override { return false; }
	bool halMasterTransmit(uint8_t* p, uint16_t size) override {
		const bool ok = chip.write(p, size);
		BusTransport::onTransferComplete(&g_i2cBus, !ok);	// what the I2C interrupt, or error interrupt, does
		return true;
	}
	bool halMasterReceive(uint8_t* p, uint16_t size) override {
		const bool ok = chip.read(p, size);
		BusTransport::onTransferComplete(&g_i2cBus, !ok);
		return true;
	}
	bool halIsDeviceReady(uint32_t, uint32_t) override { return chip.present; }
private:
	MockChip& chip;
};

// ---- harness ----
static int g_failures = 0;
static void check(bool ok, const char* what) {
	std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok) ++g_failures;
}

// Runs main() once per ms, like a bare metal loop. Returns readings seen.
template <typename TSensor>
int run(TSensor& s, MockChip& chip, uint32_t start, uint32_t ms, float* heading = nullptr, int16_t* raw = nullptr) {
	int readings = 0;
	for (uint32_t i = 0; i < ms; ++i) {
		chip.now = start + i;
		s.main(chip.now);
		if (s.newData()) {
			float hh; int16_t rr; s.getHeading(&hh); s.getRaw(&rr);
			if (heading) *heading = hh;
			if (raw) *raw = rr;
			++readings;
		}
	}
	return readings;
}

static const hmc6352_param_t kStandby = {hmc6352_standby, hmc6352_1hz, hmc6352_per_en, hmc6352_heading_mode};

template <typename TTransport>
void normalRun(const char* name) {
	std::printf("%s\n", name);
	MockChip chip;
	hmc6352<TTransport> sensor(kStandby, chip);
	float heading = 0; sensor.getHeading(&heading);
	check(std::isnan(heading), "NAN before the first reading");
	int n = run(sensor, chip, 0, 3500, &heading);
	std::printf("        heading = %.1f degrees   (%d readings in 3.5 s)\n", heading, n);
	check(std::fabs(heading - 123.4f) < 0.001f, "heading 123.4 degrees");
	check(n == 3, "a reading about every second");
	check(chip.commands.size() >= 2 && chip.commands[0] == std::vector<uint8_t>({0x47, 0x74, 0x10}), "first command: write RAM 0x74 = standby, periodic set/reset on (G 74 10)");
	check(chip.commands[1] == std::vector<uint8_t>({0x47, 0x4E, 0x00}), "second: write RAM 0x4E = heading output (G 4E 00)");
	check(chip.ramWrites == 2, "the two settings are written once each");
	check(chip.getDatas == n && chip.reads == n, "one get data command (A) and one 2 byte read per reading");
	check(chip.earlyReads == 0, "never read before the conversion time is up");
}

int main() {
	normalRun<MockBus>("hmc6352<MockBus>: bare ISensorTransport, asynchronous");
	normalRun<LoopI2C>("hmc6352<LoopI2C>: itransport's I2CTransport");

	std::printf("range of headings\n");
	{
		bool ok = true;
		for (int16_t v : {(int16_t)0, (int16_t)1, (int16_t)900, (int16_t)3599}) {
			MockChip chip; chip.value = v;
			hmc6352<LoopI2C> sensor(kStandby, chip);
			float heading = NAN; run(sensor, chip, 0, 1200, &heading);
			if (std::fabs(heading - v / 10.0f) > 0.001f) ok = false;
		}
		check(ok, "0, 0.1, 90 and 359.9 degrees all decode");
	}
	{
		MockChip chip; chip.value = 4000;
		hmc6352<LoopI2C> sensor(kStandby, chip);
		float heading = 0;
		check(run(sensor, chip, 0, 3000, &heading) == 0, "a value past 359.9 degrees is not accepted as a heading");
	}

	std::printf("query mode\n");
	{
		MockChip chip;
		hmc6352_param_t p = kStandby; p._op_mode = hmc6352_query; p.per = hmc6352_per_dis;
		hmc6352<LoopI2C> sensor(p, chip);
		float heading = NAN;
		const int n = run(sensor, chip, 0, 2500, &heading);
		check(chip.opMode == 0x01, "RAM 0x74 = query, periodic set/reset off (0x01)");
		check(n == 2 && chip.getDatas == 2 && std::fabs(heading - 123.4f) < 0.001f, "measured the same way as standby: get data, wait, read");
	}

	std::printf("continuous mode\n");
	{
		MockChip chip;
		hmc6352_param_t p = {hmc6352_continuous, hmc6352_10hz, hmc6352_per_en, hmc6352_heading_mode};
		hmc6352<LoopI2C> sensor(p, chip);
		float heading = NAN;
		const int n = run(sensor, chip, 0, 3500, &heading);
		check(chip.opMode == 0x52, "RAM 0x74 = continuous, 10 Hz, periodic set/reset on (0x52)");
		check(n == 3 && chip.getDatas == 0 && std::fabs(heading - 123.4f) < 0.001f, "the latest heading is read each second, with no get data command");
	}

	std::printf("raw magnetometer output\n");
	{
		MockChip chip; chip.value = -321;
		hmc6352_param_t p = kStandby; p.output_mode = hmc6352_raw_magnetometer_x_mode;
		hmc6352<LoopI2C> sensor(p, chip);
		float heading = 0; int16_t raw = 0;
		const int n = run(sensor, chip, 0, 1200, &heading, &raw);
		check(chip.outputMode == 0x01, "RAM 0x4E = raw magnetometer X (0x01)");
		check(n == 1 && raw == -321, "getRaw() gives the signed count");
		check(std::isnan(heading), "getHeading() gives NAN, as the value is not a heading");
	}

	std::printf("startMeasurement(): a reading on request\n");
	{
		MockChip chip;
		hmc6352<LoopI2C> sensor(kStandby, chip);
		run(sensor, chip, 0, 1100);
		const int before = chip.getDatas;
		sensor.startMeasurement();
		check(run(sensor, chip, 1100, 100) == 1 && chip.getDatas == before + 1, "taken at once: a reading within 100 ms");
		const int later = run(sensor, chip, 1200, 2500);
		check(later == 2, "one request gives one extra reading, then back to one a second");
	}

	std::printf("device absent, then plugged in\n");
	{
		MockChip chip; chip.present = false;
		hmc6352<LoopI2C> sensor(kStandby, chip);
		check(run(sensor, chip, 0, 2000) == 0, "no readings while absent");
		chip.present = true;
		check(run(sensor, chip, 2000, 3000) >= 1, "recovers by itself once the device answers");
		check(chip.ramWrites == 2, "and the settings are written again first");
	}

	std::printf("operation that never completes\n");
	{
		MockChip chip;
		hmc6352<MockBus> sensor(kStandby, chip);
		run(sensor, chip, 0, 1010);
		chip.stuck = true;
		run(sensor, chip, 1010, 2000);
		check(sensor.state() == hmc6352_error_state, "gives up after the bus timeout");
		chip.stuck = false;
		check(run(sensor, chip, 3010, 3000) >= 1, "recovers when the bus comes back");
	}

	std::printf("transport that cannot start an operation\n");
	{
		MockChip chip;
		hmc6352<MockBus> sensor(kStandby, chip);
		run(sensor, chip, 0, 500);
		size_t before = chip.commands.size();
		chip.refuse = true;
		check(run(sensor, chip, 500, 2500) == 0 && chip.commands.size() == before, "nothing reaches the bus");
		chip.refuse = false;
		check(run(sensor, chip, 3000, 3000) >= 1, "recovers when the bus is free");
	}

	std::printf("tick counter rollover\n");
	{
		MockChip chip;
		hmc6352<LoopI2C> sensor(kStandby, chip);
		const uint32_t start = 0xFFFFFFFFu - 2500;	// wraps 2.5 s into the run
		check(run(sensor, chip, start, 6000) == 5, "readings keep their rhythm across the wrap");
	}

	std::printf("xhmc6352: sleep times under an OS\n");
	{
		MockChip chip;
		xhmc6352<MockBus> sensor(kStandby, chip);
		// Like a thread: main(), then advance the clock by however long
		// main() slept, plus the one tick the loop gives up every cycle.
		uint32_t now = 0; int calls = 0, readings = 0; uint32_t longest = 0;
		g_delays.clear();
		while (now < 10000) {
			chip.now = now; g_slept = 0;
			sensor.main(now); ++calls;
			if (sensor.newData()) { float v; sensor.getHeading(&v); ++readings; }
			now += g_slept + 1;
		}
		for (uint32_t d : g_delays) if (d > longest) longest = d;
		std::printf("        %d readings in 10 s from %d calls to main(), longest sleep %u ms\n", readings, calls, (unsigned)longest);
		check(readings >= 8 && chip.earlyReads == 0, "still measures on schedule, and never reads early");
		check(longest == 100, "never sleeps longer than 100 ms, so a startMeasurement() request is seen promptly");
		check(calls < 300, "wakes only when there is something to do");
	}

	std::printf("\n%s\n", g_failures ? "FAILED" : "all passed");
	return g_failures ? 1 : 0;
}
