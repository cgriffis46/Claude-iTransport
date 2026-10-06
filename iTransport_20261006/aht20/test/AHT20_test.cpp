/*
 * AHT20_test.cpp
 *
 * Host test for AHT20<TTransport>. No hardware, HAL or RTOS needed:
 *
 *   g++ -std=c++17 -Wall -Wextra -I../inc -Istub \
 *       -I<safeTransport>/itransport/inc -I<safeTransport>/isensor/inc \
 *       AHT20_test.cpp <safeTransport>/itransport/src/BusTransport.cpp \
 *       <safeTransport>/itransport/src/I2CTransport.cpp -o AHT20_test
 */

#include <cstdio>
#include <cmath>
#include <vector>
#include "I2CTransport.h"
#include "xAHT20.h"	// pulls in AHT20.h. stub/cmsis_os2.h stands in for the RTOS

uint32_t g_slept = 0;						// set by the stub osDelay()
std::vector<uint32_t> g_delays;

// CRC-8, polynomial 0x31, initial value 0xFF, as the AHT20 appends to a reading.
static uint8_t crc8(const uint8_t* p, int n) {
	uint8_t crc = 0xFF;
	for (int i = 0; i < n; ++i) {
		crc ^= p[i];
		for (int k = 0; k < 8; ++k) crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
	}
	return crc;
}

// ---- simulated AHT20: takes commands, is read back as a run of bytes ----
struct MockChip {
	uint32_t now = 0;						// the test's clock
	bool present = true;
	bool stuck = false;						// operations never complete
	bool refuse = false;					// transport cannot start an operation
	bool corrupt = false;					// one bit of a reading flipped on the way back
	bool calibratedAfterReset = true;		// most chips come up calibrated
	bool neverCalibrates = false;
	uint8_t extraStatusBits = 0x00;			// other bits a chip may set alongside the calibration bits
	uint32_t conversionTime = 80;
	float humidity = 50.0f, temperature = 25.0f;

	bool calibrated = false;
	bool measuring = false; uint32_t convEnd = 0;
	bool statusAsked = false;
	int resets = 0, inits = 0, measurements = 0, statusReads = 0;
	std::vector<std::vector<uint8_t>> commands;

	uint8_t status() {
		if (measuring && (int32_t)(now - convEnd) >= 0) measuring = false;
		return (uint8_t)((calibrated ? 0x18 : 0x00) | extraStatusBits | (measuring ? 0x80 : 0x00));
	}
	bool write(const uint8_t* b, uint16_t n) {			// false: not acknowledged
		if (!present) return false;
		commands.emplace_back(b, b + n);
		if (n == 1 && b[0] == 0xBA) { ++resets; calibrated = calibratedAfterReset && !neverCalibrates; measuring = false; }
		else if (n == 1 && b[0] == 0x71) { statusAsked = true; }
		else if (n == 3 && b[0] == 0xBE && b[1] == 0x08 && b[2] == 0x00) { ++inits; calibrated = !neverCalibrates; }
		else if (n == 3 && b[0] == 0xAC && b[1] == 0x33 && b[2] == 0x00) { ++measurements; measuring = true; convEnd = now + conversionTime; }
		return true;
	}
	bool read(uint8_t* out, uint16_t n) {
		if (!present) return false;
		if (n == 1) { ++statusReads; out[0] = status(); return true; }
		const uint32_t h = (uint32_t)lroundf(humidity / 100.0f * 1048576.0f);
		const uint32_t t = (uint32_t)lroundf((temperature + 50.0f) / 200.0f * 1048576.0f);
		uint8_t r[7];
		r[0] = status();
		r[1] = (uint8_t)(h >> 12); r[2] = (uint8_t)(h >> 4);
		r[3] = (uint8_t)(((h & 0x0F) << 4) | ((t >> 16) & 0x0F));
		r[4] = (uint8_t)(t >> 8); r[5] = (uint8_t)t;
		r[6] = crc8(r, 6);
		if (corrupt) r[4] ^= 0x10;
		for (uint16_t i = 0; i < n && i < 7; ++i) out[i] = r[i];
		return true;
	}
	int count(std::vector<uint8_t> c) const { int n = 0; for (auto& x : commands) if (x == c) ++n; return n; }
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
// The AHT20 goes through HAL_I2C_Master_Transmit_IT / Master_Receive_IT,
// never the register (Mem) calls.
static int g_i2cBus;	// stands in for &hi2c1
static int g_memCalls = 0;
class LoopI2C : public I2CTransport {
public:
	explicit LoopI2C(MockChip& c) : I2CTransport(&g_i2cBus, AHT20_I2C_Addr, nullptr), chip(c) {}
protected:
	bool halMemWrite(uint8_t, uint8_t*, uint16_t) override { ++g_memCalls; return false; }
	bool halMemRead(uint8_t, uint8_t*, uint16_t) override { ++g_memCalls; return false; }
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
int run(TSensor& s, MockChip& chip, uint32_t start, uint32_t ms, float* t = nullptr, float* h = nullptr) {
	int readings = 0;
	for (uint32_t i = 0; i < ms; ++i) {
		chip.now = start + i;
		s.main(chip.now);
		if (s.newData()) {
			float tt, hh; s.getTempHumidity(&tt, &hh);
			if (t) *t = tt;
			if (h) *h = hh;
			++readings;
		}
	}
	return readings;
}

template <typename TTransport>
void normalRun(const char* name) {
	std::printf("%s\n", name);
	MockChip chip;
	AHT20<TTransport> sensor(chip);
	float t = 0, h = 0; sensor.getTempHumidity(&t, &h);
	check(std::isnan(t) && std::isnan(h), "NAN before the first reading");
	int n = run(sensor, chip, 0, 3500, &t, &h);
	std::printf("        T = %.3f C   H = %.3f %%RH   (%d readings in 3.5 s)\n", t, h, n);
	check(std::fabs(t - 25.0f) < 0.001f && std::fabs(h - 50.0f) < 0.001f, "temperature 25 C and humidity 50 %RH");
	check(n == 3, "a reading about every 1.1 s: the period, then the measurement");
	check(chip.resets == 1 && chip.count({0xBA}) == 1, "soft reset command (BA) sent once");
	check(chip.count({0x71}) == 1 && chip.statusReads == 1, "status asked for (71) and read as one byte");
	check(chip.inits == 0, "no initialisation command, as the chip reported itself calibrated");
	check(chip.count({0xAC, 0x33, 0x00}) == n, "one measurement command (AC 33 00) per reading");
}

int main() {
	normalRun<MockBus>("AHT20<MockBus>: bare ISensorTransport, asynchronous");
	normalRun<LoopI2C>("AHT20<LoopI2C>: itransport's I2CTransport");
	check(g_memCalls == 0, "only the command-style HAL calls were used, never the register ones");

	std::printf("values across the range\n");
	{
		const struct { float t, h; } cases[] = {{-40.0f, 0.0f}, {-10.5f, 12.25f}, {0.0f, 99.5f}, {85.0f, 100.0f - 0.0001f}};
		bool ok = true;
		for (const auto& c : cases) {
			MockChip chip; chip.temperature = c.t; chip.humidity = c.h;
			AHT20<LoopI2C> sensor(chip);
			float t = NAN, h = NAN; run(sensor, chip, 0, 1500, &t, &h);
			if (std::fabs(t - c.t) > 0.001f || std::fabs(h - c.h) > 0.001f) ok = false;
		}
		check(ok, "-40 C to 85 C and 0 to 100 %RH all decode");
	}

	std::printf("chip that comes up uncalibrated\n");
	{
		MockChip chip; chip.calibratedAfterReset = false;
		AHT20<LoopI2C> sensor(chip);
		check(run(sensor, chip, 0, 2000) >= 1, "readings arrive");
		check(chip.count({0xBE, 0x08, 0x00}) == 1 && chip.resets == 1, "after one initialisation command (BE 08 00)");
	}
	{
		MockChip chip; chip.extraStatusBits = 0x04;		// reports 0x1C, not exactly 0x18
		AHT20<LoopI2C> sensor(chip);
		check(run(sensor, chip, 0, 2000) >= 1 && chip.inits == 0, "status 0x1C counts as calibrated: only the two calibration bits are tested");
	}
	{
		MockChip chip; chip.neverCalibrates = true;
		AHT20<MockBus> sensor(chip);
		check(run(sensor, chip, 0, 5000) == 0, "chip that never calibrates: no readings");
		check(chip.resets >= 2 && chip.inits >= 4, "it is re-initialised a bounded number of times, then reset and tried again");
	}

	std::printf("conversion slower than the first wait\n");
	{
		MockChip chip; chip.conversionTime = 130;
		AHT20<LoopI2C> sensor(chip);
		float t = NAN, h = NAN;
		check(run(sensor, chip, 0, 2500, &t, &h) >= 1 && std::fabs(t - 25.0f) < 0.001f, "a busy reply is read again, not treated as an error");
		check(chip.resets == 1, "without resetting the chip");
	}

	std::printf("reading corrupted on the way back\n");
	{
		MockChip chip; chip.corrupt = true;
		AHT20<LoopI2C> sensor(chip);
		check(run(sensor, chip, 0, 4000) == 0 && chip.measurements >= 2, "CRC failure: reading rejected");
		chip.corrupt = false;
		float nt = 0, nh = 0; sensor.getTempHumidity(&nt, &nh);
		check(std::isnan(nt) && std::isnan(nh), "and the values read as NAN, not as an old reading");
		check(run(sensor, chip, 4000, 3000) >= 1, "next good read is accepted");
	}

	std::printf("StartMeasurement(): a measurement on request\n");
	{
		MockChip chip;
		AHT20<LoopI2C> sensor(chip);
		run(sensor, chip, 0, 1400);								// first periodic reading done at about 1.2 s
		const int before = chip.measurements;
		sensor.StartMeasurement();
		const int soon = run(sensor, chip, 1400, 200);
		check(soon == 1 && chip.measurements == before + 1, "starts at once: a reading within 200 ms");
		const int later = run(sensor, chip, 1600, 3000);
		check(later >= 2 && later <= 3, "one request gives one extra measurement, then back to one a second");
	}

	std::printf("device absent, then plugged in\n");
	{
		MockChip chip; chip.present = false;
		AHT20<LoopI2C> sensor(chip);
		check(run(sensor, chip, 0, 2000) == 0, "no readings while absent");
		chip.present = true;
		check(run(sensor, chip, 2000, 3000) >= 1, "recovers by itself once the device answers");
	}

	std::printf("operation that never completes\n");
	{
		MockChip chip;
		AHT20<MockBus> sensor(chip);
		run(sensor, chip, 0, 1150);
		chip.stuck = true;
		run(sensor, chip, 1150, 2000);
		check(sensor.state() == aht_error, "gives up after the bus timeout");
		float nt = 0, nh = 0; sensor.getTempHumidity(&nt, &nh);
		check(std::isnan(nt) && std::isnan(nh), "the earlier reading is no longer offered as current");
		chip.stuck = false;
		check(run(sensor, chip, 3150, 3000) >= 1, "recovers when the bus comes back");
	}

	std::printf("transport that cannot start an operation\n");
	{
		MockChip chip;
		AHT20<MockBus> sensor(chip);
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
		AHT20<LoopI2C> sensor(chip);
		const uint32_t start = 0xFFFFFFFFu - 2500;	// wraps 2.5 s into the run
		check(run(sensor, chip, start, 6000) == 5, "readings keep their rhythm across the wrap");
	}

	std::printf("xAHT20: sleep times under an OS\n");
	{
		MockChip chip;
		xAHT20<MockBus> sensor(chip);
		// Like a thread: main(), then advance the clock by however long
		// main() slept, plus the one tick the loop gives up every cycle.
		uint32_t now = 0; int calls = 0, readings = 0; uint32_t longest = 0;
		g_delays.clear();
		while (now < 10000) {
			chip.now = now; g_slept = 0;
			sensor.main(now); ++calls;
			if (sensor.newData()) ++readings;
			now += g_slept + 1;
		}
		for (uint32_t d : g_delays) if (d > longest) longest = d;
		std::printf("        %d readings in 10 s from %d calls to main(), longest sleep %u ms\n", readings, calls, (unsigned)longest);
		check(readings >= 8, "still measures on schedule");
		check(longest == 100, "never sleeps longer than 100 ms, so a StartMeasurement() request is seen promptly");
		check(calls < 300, "wakes only when there is something to do");
	}

	std::printf("\n%s\n", g_failures ? "FAILED" : "all passed");
	return g_failures ? 1 : 0;
}
