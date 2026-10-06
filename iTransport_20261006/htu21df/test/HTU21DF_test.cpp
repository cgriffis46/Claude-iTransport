/*
 * HTU21DF_test.cpp
 *
 * Host test for HTU21DF<TTransport>. No hardware, HAL or RTOS needed:
 *
 *   g++ -std=c++17 -Wall -Wextra -I../inc -Istub \
 *       -I<safeTransport>/itransport/inc -I<safeTransport>/isensor/inc \
 *       HTU21DF_test.cpp <safeTransport>/itransport/src/BusTransport.cpp \
 *       <safeTransport>/itransport/src/I2CTransport.cpp -o HTU21DF_test
 */

#include <cstdio>
#include <cmath>
#include <vector>
#include "I2CTransport.h"
#include "xHTU21DF.h"	// pulls in HTU21DF.h. stub/cmsis_os2.h stands in for the RTOS

uint32_t g_slept = 0;						// set by the stub osDelay()
std::vector<uint32_t> g_delays;

// CRC-8, polynomial 0x31, initial value 0x00, as the HTU21D-F appends to a measurement.
static uint8_t crc8(const uint8_t* p, int n) {
	uint8_t crc = 0x00;
	for (int i = 0; i < n; ++i) {
		crc ^= p[i];
		for (int k = 0; k < 8; ++k) crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
	}
	return crc;
}

// ---- simulated HTU21D-F: takes one byte commands, is read back as a run of bytes ----
struct MockChip {
	uint32_t now = 0;						// the test's clock
	bool present = true;
	bool stuck = false;						// operations never complete
	bool refuse = false;					// transport cannot start an operation
	bool corrupt = false;					// one bit of a measurement flipped on the way back
	float temperature = 21.5f, humidity = 40.0f;

	uint8_t pending = 0;					// measurement command waiting to be read
	uint32_t convEnd = 0;
	int resets = 0, tCommands = 0, hCommands = 0;
	std::vector<uint8_t> commands;

	bool write(const uint8_t* b, uint16_t n) {			// false: not acknowledged
		if (!present || n != 1) return false;
		commands.push_back(b[0]);
		switch (b[0]) {
		case 0xFE: ++resets; pending = 0; break;
		case 0xE3: ++tCommands; pending = 0xE3; convEnd = now + 44; break;	// 14 bit temperature takes up to 50 ms
		case 0xE5: ++hCommands; pending = 0xE5; convEnd = now + 14; break;
		default: break;
		}
		return true;
	}
	bool read(uint8_t* out, uint16_t n) {
		if (!present || n != 3 || pending == 0) return false;
		if ((int32_t)(now - convEnd) < 0) return false;	// still converting: no acknowledge
		uint16_t code;
		if (pending == 0xE3) {
			code = (uint16_t)lroundf((temperature + 46.85f) * 65536.0f / 175.72f);
			code = (uint16_t)(code & 0xFFFC);			// status bits: 00 for temperature
		} else {
			code = (uint16_t)lroundf((humidity + 6.0f) * 65536.0f / 125.0f);
			code = (uint16_t)((code & 0xFFFC) | 0x0002);	// status bits: 10 for humidity
		}
		pending = 0;
		out[0] = code >> 8; out[1] = code & 0xFF; out[2] = crc8(out, 2);	// the CRC covers the status bits too
		if (corrupt) out[0] ^= 0x04;
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
	explicit LoopI2C(MockChip& c) : I2CTransport(&g_i2cBus, HTU21DF_I2CADDR, nullptr), chip(c) {}
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
int run(TSensor& s, MockChip& chip, uint32_t start, uint32_t ms, float* t = nullptr, float* h = nullptr) {
	int readings = 0;
	for (uint32_t i = 0; i < ms; ++i) {
		chip.now = start + i;
		s.main(chip.now);
		if (s.newData()) {
			float tt, hh; s.readTemperature(&tt); s.readHumidity(&hh);
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
	HTU21DF<TTransport> sensor(chip);
	float t = 0, h = 0;
	check(!sensor.readTemperature(&t) && std::isnan(t), "NAN, and not new, before the first reading");
	int n = run(sensor, chip, 0, 3600, &t, &h);
	std::printf("        T = %.3f C   H = %.3f %%RH   (%d readings in 3.6 s)\n", t, h, n);
	check(std::fabs(t - 21.5f) < 0.01f && std::fabs(h - 40.0f) < 0.01f, "temperature 21.5 C and humidity 40 %RH");
	check(n == 3, "a reading about every 1.1 s: the period, then the two measurements");
	check(chip.resets == 1 && chip.tCommands == n && chip.hCommands == n, "one reset, then a temperature and a humidity command per reading");
	check(chip.commands[0] == 0xFE && chip.commands[1] == 0xE3 && chip.commands[2] == 0xE5, "commands: reset, temperature, humidity");
}

int main() {
	normalRun<MockBus>("HTU21DF<MockBus>: bare ISensorTransport, asynchronous");
	normalRun<LoopI2C>("HTU21DF<LoopI2C>: itransport's I2CTransport");

	std::printf("values below zero\n");
	{
		MockChip chip; chip.temperature = -20.25f; chip.humidity = 2.5f;
		HTU21DF<LoopI2C> sensor(chip);
		float t = NAN, h = NAN; run(sensor, chip, 0, 1500, &t, &h);
		check(std::fabs(t - (-20.25f)) < 0.01f && std::fabs(h - 2.5f) < 0.01f, "-20.25 C and 2.5 %RH");
	}

	std::printf("readTemperature() and readHumidity() say whether the value is new\n");
	{
		MockChip chip;
		HTU21DF<LoopI2C> sensor(chip);
		for (uint32_t i = 0; i < 1500 && !sensor.newData(); ++i) { chip.now = i; sensor.main(i); }
		float t = NAN, h = NAN;
		check(sensor.readTemperature(&t), "true the first time after a measurement");
		check(!sensor.readHumidity(&h) && !sensor.readTemperature(&t) && !std::isnan(h), "false after that, with the value still returned");
	}

	std::printf("measurement on request\n");
	{
		MockChip chip;
		HTU21DF<LoopI2C> sensor(chip);
		run(sensor, chip, 0, 1300);						// first periodic reading done at about 1.1 s
		const int before = chip.tCommands;
		sensor.startTempMeasurement();
		check(run(sensor, chip, 1300, 200) == 1 && chip.tCommands == before + 1 && chip.hCommands == before + 1, "startTempMeasurement(): both measured at once, within 200 ms");
		sensor.startHumidityMeasurement();
		check(run(sensor, chip, 1500, 200) == 1, "startHumidityMeasurement(): the same");
		const int later = run(sensor, chip, 1700, 2500);
		check(later == 2, "each request is taken once, then back to one reading every period");
	}

	std::printf("measurement corrupted on the way back\n");
	{
		MockChip chip; chip.corrupt = true;
		HTU21DF<LoopI2C> sensor(chip);
		check(run(sensor, chip, 0, 4000) == 0 && chip.tCommands >= 2, "CRC failure: reading rejected, and tried again");
		chip.corrupt = false;
		check(run(sensor, chip, 4000, 3000) >= 1, "next good read is accepted");
	}

	std::printf("device absent, then plugged in\n");
	{
		MockChip chip; chip.present = false;
		HTU21DF<LoopI2C> sensor(chip);
		check(run(sensor, chip, 0, 2000) == 0, "no readings while absent");
		chip.present = true;
		check(run(sensor, chip, 2000, 3000) >= 1, "recovers by itself once the device answers");
	}

	std::printf("device unplugged during a measurement\n");
	{
		MockChip chip;
		HTU21DF<LoopI2C> sensor(chip);
		run(sensor, chip, 0, 1030);						// temperature conversion under way
		chip.present = false;
		check(run(sensor, chip, 1030, 3000) == 0, "the read that is not acknowledged ends in the error state, not a tight retry");
		check(sensor.state() == htu21df_error_state || sensor.state() == htu21df_init_state, "and it keeps looking for the device");
	}

	std::printf("operation that never completes\n");
	{
		MockChip chip;
		HTU21DF<MockBus> sensor(chip);
		run(sensor, chip, 0, 1100);
		chip.stuck = true;
		run(sensor, chip, 1100, 2000);
		check(sensor.state() == htu21df_error_state, "gives up after the bus timeout");
		chip.stuck = false;
		check(run(sensor, chip, 3100, 3000) >= 1, "recovers when the bus comes back");
	}

	std::printf("transport that cannot start an operation\n");
	{
		MockChip chip;
		HTU21DF<MockBus> sensor(chip);
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
		HTU21DF<LoopI2C> sensor(chip);
		const uint32_t start = 0xFFFFFFFFu - 2500;	// wraps 2.5 s into the run
		check(run(sensor, chip, start, 6000) == 5, "readings keep their rhythm across the wrap");
	}

	std::printf("xHTU21DF: sleep times under an OS\n");
	{
		MockChip chip;
		xHTU21DF<MockBus> sensor(chip);
		// Like a thread: main(), then advance the clock by however long
		// main() slept, plus the one tick the loop gives up every cycle.
		uint32_t now = 0; int calls = 0, readings = 0; uint32_t longest = 0;
		g_delays.clear();
		while (now < 10000) {
			chip.now = now; g_slept = 0;
			sensor.main(now); ++calls;
			if (sensor.newData()) { float v; sensor.readTemperature(&v); ++readings; }
			now += g_slept + 1;
		}
		for (uint32_t d : g_delays) if (d > longest) longest = d;
		std::printf("        %d readings in 10 s from %d calls to main(), longest sleep %u ms\n", readings, calls, (unsigned)longest);
		check(readings >= 8, "still measures on schedule");
		check(longest == 100, "never sleeps longer than 100 ms, so a request is seen promptly");
		check(calls < 300, "wakes only when there is something to do");
	}

	std::printf("\n%s\n", g_failures ? "FAILED" : "all passed");
	return g_failures ? 1 : 0;
}
