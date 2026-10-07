/*
 * Si7021_test.cpp
 *
 * Host test for Si7021<TTransport>. No hardware, HAL or RTOS needed:
 *
 *   g++ -std=c++17 -Wall -Wextra -I../inc -Istub \
 *       -I<safeTransport>/itransport/inc -I<safeTransport>/isensor/inc \
 *       Si7021_test.cpp <safeTransport>/itransport/src/BusTransport.cpp \
 *       <safeTransport>/itransport/src/I2CTransport.cpp -o Si7021_test
 */

#include <cstdio>
#include <cmath>
#include <vector>
#include "I2CTransport.h"
#include "xSi7021.h"	// pulls in Si7021.h. stub/cmsis_os2.h stands in for the RTOS

uint32_t g_slept = 0;						// set by the stub osDelay()
std::vector<uint32_t> g_delays;

// CRC-8, polynomial 0x31, initial value 0x00, as the Si7021 appends to a measurement.
static uint8_t crc8(const uint8_t* p, int n) {
	uint8_t crc = 0x00;
	for (int i = 0; i < n; ++i) {
		crc ^= p[i];
		for (int k = 0; k < 8; ++k) crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
	}
	return crc;
}

// ---- simulated Si7021 ----
struct MockChip {
	uint32_t now = 0;						// the test's clock
	bool present = true;
	bool stuck = false;						// operations never complete
	bool refuse = false;					// transport cannot start an operation
	bool corrupt = false;					// one bit of a measurement flipped on the way back
	float temperature = 22.25f, humidity = 45.5f;

	uint8_t reg1 = 0x3A;					// user register 1, power-on value
	uint8_t heaterReg = 0x00;
	uint8_t pending = 0;					// measurement command waiting to be read: 0xF5 or 0xF3
	uint32_t convEnd = 0;
	int resets = 0, rhCommands = 0, tCommands = 0, reg1Writes = 0, heaterWrites = 0;

	// one-byte command, or command + value
	bool write(const uint8_t* b, uint16_t n) {			// false: not acknowledged
		if (!present || n < 1) return false;
		switch (b[0]) {
		case 0xFE: ++resets; reg1 = 0x3A; heaterReg = 0; pending = 0; break;
		case 0xF5: ++rhCommands; pending = 0xF5; convEnd = now + 20; break;
		case 0xF3: ++tCommands; pending = 0xF3; convEnd = now + 20; break;
		case 0xE6: if (n == 2) { reg1 = b[1]; ++reg1Writes; } break;
		case 0x51: if (n == 2) { heaterReg = b[1]; ++heaterWrites; } break;
		default: break;
		}
		return true;
	}
	// read straight after a command byte (repeated start): the two settings registers
	bool readAfter(uint8_t cmd, uint8_t* out, uint16_t n) {
		if (!present || n != 1) return false;
		if (cmd == 0xE7) { out[0] = reg1; return true; }
		if (cmd == 0x11) { out[0] = heaterReg; return true; }
		return false;
	}
	// plain read: a measurement result
	bool read(uint8_t* out, uint16_t n) {
		if (!present || n != 3 || pending == 0) return false;
		if ((int32_t)(now - convEnd) < 0) return false;	// still converting: no acknowledge
		const uint16_t code = (pending == 0xF5)
			? (uint16_t)lroundf((humidity + 6.0f) * 65536.0f / 125.0f)
			: (uint16_t)lroundf((temperature + 46.85f) * 65536.0f / 175.72f);
		pending = 0;
		out[0] = code >> 8; out[1] = code & 0xFF; out[2] = crc8(out, 2);
		if (corrupt) out[1] ^= 0x10;
		return true;
	}
};

// ---- transport 1: a bare ISensorTransport whose operations take time ----
class MockBus : public ISensorTransport {
public:
	explicit MockBus(MockChip& c) : chip(c) {}
	bool writeReg(uint8_t r, uint8_t v) override { const uint8_t b[2] = {r, v}; return writeBytes(b, 2); }
	bool writeRegs(uint8_t, const uint8_t*, uint8_t) override { return false; }	// not used by this driver
	bool readRegs(uint8_t r, uint8_t* buf, uint8_t len) override { return begin(buf, len, true, r); }
	bool writeBytes(const uint8_t* buf, uint8_t len) override {
		if (busy || chip.refuse) return false;
		failed = !chip.write(buf, len);
		busy = true; polls = 1; dest = nullptr;
		return true;
	}
	bool readBytes(uint8_t* buf, uint8_t len) override { return begin(buf, len, false, 0); }
	bool isBusy() const override {
		if (!busy) return false;
		if (chip.stuck) return true;
		if (polls > 0) { --polls; return true; }
		if (dest && !(isReg ? chip.readAfter(reg, dest, n) : chip.read(dest, n))) failed = true;	// lands only now
		busy = false;
		return false;
	}
	bool lastOpFailed() const override { return failed; }
	bool checkDevice() override { return chip.present; }
private:
	bool begin(uint8_t* buf, uint8_t len, bool asReg, uint8_t r) {
		if (busy || chip.refuse) return false;
		failed = false; busy = true; polls = 1; dest = buf; n = len; isReg = asReg; reg = r;
		return true;
	}
	MockChip& chip;
	mutable bool busy = false; mutable int polls = 0; mutable bool failed = false;
	uint8_t* dest = nullptr; uint8_t n = 0; bool isReg = false; uint8_t reg = 0;
};

// ---- transport 2: itransport's real I2CTransport, with the HAL calls simulated ----
static int g_i2cBus;	// stands in for &hi2c1
class LoopI2C : public I2CTransport {
public:
	explicit LoopI2C(MockChip& c) : I2CTransport(&g_i2cBus, SI7021_I2C_ADDR, nullptr), chip(c) {}
protected:
	bool halMemWrite(uint8_t reg, uint8_t* p, uint16_t size) override {		// the settings register writes
		uint8_t b[2] = {reg, p[0]};
		const bool ok = (size == 1) && chip.write(b, 2);
		BusTransport::onTransferComplete(&g_i2cBus, !ok);	// what the I2C interrupt, or error interrupt, does
		return true;
	}
	bool halMemRead(uint8_t reg, uint8_t* p, uint16_t size) override {		// the settings register reads
		const bool ok = chip.readAfter(reg, p, size);
		BusTransport::onTransferComplete(&g_i2cBus, !ok);
		return true;
	}
	bool halMasterTransmit(uint8_t* p, uint16_t size) override {			// reset and measurement commands
		const bool ok = chip.write(p, size);
		BusTransport::onTransferComplete(&g_i2cBus, !ok);
		return true;
	}
	bool halMasterReceive(uint8_t* p, uint16_t size) override {				// measurement results
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
			float tt, hh; s.readTempHumidity(&tt, &hh);
			if (t) *t = tt;
			if (h) *h = hh;
			++readings;
		}
	}
	return readings;
}

static const Si7021_param_t kParam = {Si7021_heater_disable, 0, Si7021_RH12_T14};

template <typename TTransport>
void normalRun(const char* name) {
	std::printf("%s\n", name);
	MockChip chip;
	Si7021<TTransport> sensor(kParam, chip);
	float t = 0, h = 0; sensor.readTempHumidity(&t, &h);
	check(std::isnan(t) && std::isnan(h), "NAN before the first reading");
	int n = run(sensor, chip, 0, 7000, &t, &h);
	std::printf("        T = %.3f C   H = %.3f %%RH   (%d readings in 7 s)\n", t, h, n);
	check(std::fabs(t - 22.25f) < 0.005f && std::fabs(h - 45.5f) < 0.005f, "temperature 22.25 C and humidity 45.5 %RH");
	check(n == 3, "a reading about every 2.2 s: the period, then the two measurements");
	check(chip.resets == 1, "reset command sent once");
	check(chip.rhCommands == n && chip.tCommands == n, "one humidity and one temperature command per reading");
	check(chip.reg1 == 0x3A && chip.reg1Writes == 1, "user register 1 written once, reserved bits kept as the chip had them (0x3A)");
	check(chip.heaterReg == 0x00 && chip.heaterWrites == 1, "heater register written once");
}

int main() {
	normalRun<MockBus>("Si7021<MockBus>: bare ISensorTransport, asynchronous");
	normalRun<LoopI2C>("Si7021<LoopI2C>: itransport's I2CTransport");

	std::printf("values below zero\n");
	{
		MockChip chip; chip.temperature = -12.5f; chip.humidity = 3.0f;
		Si7021<LoopI2C> sensor(kParam, chip);
		float t = NAN, h = NAN; run(sensor, chip, 0, 2500, &t, &h);
		check(std::fabs(t - (-12.5f)) < 0.005f && std::fabs(h - 3.0f) < 0.005f, "-12.5 C and 3 %RH");
	}

	std::printf("settings\n");
	{
		MockChip chip;
		Si7021_param_t p = {Si7021_heater_enable, 0x05, Si7021_RH11_T11};
		Si7021<LoopI2C> sensor(p, chip);
		run(sensor, chip, 0, 500);
		check(chip.reg1 == 0xBF, "11 bit resolution sets both resolution bits, heater on sets its bit (0xBF)");
		check(chip.heaterReg == 0x05, "heater current 5 in the heater register");
	}
	{
		MockChip chip;
		Si7021_param_t p = {Si7021_heater_disable, 0, Si7021_RH10_T13};
		Si7021<LoopI2C> sensor(p, chip);
		run(sensor, chip, 0, 500);
		check(chip.reg1 == 0xBA, "10 bit resolution sets only the upper resolution bit (0xBA)");
	}
	{
		MockChip chip;
		Si7021<LoopI2C> sensor(kParam, chip);
		run(sensor, chip, 0, 500);
		sensor.setHeater(true, 0x0A);
		run(sensor, chip, 500, 100);
		check(chip.reg1 == 0x3E && chip.heaterReg == 0x0A, "setHeater(true, 10) while running: heater bit on, current 10");
		sensor.setHeater(false, 0x00);
		run(sensor, chip, 600, 100);
		check(chip.reg1 == 0x3A && chip.heaterReg == 0x00 && chip.resets == 1, "setHeater(false, 0): off again, with no reset");
	}

	std::printf("measurements on request\n");
	{
		MockChip chip;
		Si7021<LoopI2C> sensor(kParam, chip);
		run(sensor, chip, 0, 2400);						// first periodic reading done at about 2.3 s
		int rh = chip.rhCommands, tc = chip.tCommands;
		sensor.startTempMeasurement();
		check(run(sensor, chip, 2400, 300) == 1 && chip.tCommands == tc + 1 && chip.rhCommands == rh, "startTempMeasurement(): temperature only, within 300 ms");
		rh = chip.rhCommands; tc = chip.tCommands;
		sensor.startHumidityMeasurement();
		check(run(sensor, chip, 2700, 400) == 1 && chip.rhCommands == rh + 1 && chip.tCommands == tc + 1, "startHumidityMeasurement(): humidity then temperature");
		const int later = run(sensor, chip, 3100, 4500);
		check(later == 2, "each request is taken once, then back to one reading every period");
	}

	std::printf("measurement corrupted on the way back\n");
	{
		MockChip chip;
		Si7021<LoopI2C> sensor(kParam, chip);
		run(sensor, chip, 0, 2500);
		chip.corrupt = true;
		check(run(sensor, chip, 2500, 4000) == 0, "CRC failure: reading rejected");
		float t = 0, h = 0; sensor.readTempHumidity(&t, &h);
		check(std::isnan(t) && std::isnan(h), "and the values read as NAN, not as the earlier reading");
		chip.corrupt = false;
		check(run(sensor, chip, 6500, 4000) >= 1, "next good read is accepted");
	}

	std::printf("device absent, then plugged in\n");
	{
		MockChip chip; chip.present = false;
		Si7021<LoopI2C> sensor(kParam, chip);
		check(run(sensor, chip, 0, 2000) == 0, "no readings while absent");
		chip.present = true;
		check(run(sensor, chip, 2000, 4000) >= 1, "recovers by itself once the device answers");
	}

	std::printf("operation that never completes\n");
	{
		MockChip chip;
		Si7021<MockBus> sensor(kParam, chip);
		run(sensor, chip, 0, 2150);
		chip.stuck = true;
		run(sensor, chip, 2150, 2000);
		check(sensor.state() == Si7021_err_state, "gives up after the bus timeout");
		chip.stuck = false;
		check(run(sensor, chip, 4150, 4000) >= 1, "recovers when the bus comes back");
	}

	std::printf("transport that cannot start an operation\n");
	{
		MockChip chip;
		Si7021<MockBus> sensor(kParam, chip);
		run(sensor, chip, 0, 500);
		const int before = chip.rhCommands + chip.resets + chip.reg1Writes;
		chip.refuse = true;
		check(run(sensor, chip, 500, 4000) == 0 && chip.rhCommands + chip.resets + chip.reg1Writes == before, "nothing reaches the bus");
		chip.refuse = false;
		check(run(sensor, chip, 4500, 4000) >= 1, "recovers when the bus is free");
	}

	std::printf("tick counter rollover\n");
	{
		MockChip chip;
		Si7021<LoopI2C> sensor(kParam, chip);
		const uint32_t start = 0xFFFFFFFFu - 5000;	// wraps 5 s into the run
		check(run(sensor, chip, start, 10000) == 4, "readings keep their rhythm across the wrap");
	}

	std::printf("xSi7021: sleep times under an OS\n");
	{
		MockChip chip;
		xSi7021<MockBus> sensor(kParam, chip);
		// Like a thread: main(), then advance the clock by however long
		// main() slept, plus the one tick the loop gives up every cycle.
		uint32_t now = 0; int calls = 0, readings = 0; uint32_t longest = 0;
		g_delays.clear();
		while (now < 20000) {
			chip.now = now; g_slept = 0;
			sensor.main(now); ++calls;
			if (sensor.newData()) { float a, b; sensor.readTempHumidity(&a, &b); ++readings; }
			now += g_slept + 1;
		}
		for (uint32_t d : g_delays) if (d > longest) longest = d;
		std::printf("        %d readings in 20 s from %d calls to main(), longest sleep %u ms\n", readings, calls, (unsigned)longest);
		check(readings >= 8, "still measures on schedule");
		check(longest == 100, "never sleeps longer than 100 ms, so a request is seen promptly");
		check(calls < 400, "wakes only when there is something to do");
	}

	std::printf("\n%s\n", g_failures ? "FAILED" : "all passed");
	return g_failures ? 1 : 0;
}
