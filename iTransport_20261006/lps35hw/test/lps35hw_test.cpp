/*
 * lps35hw_test.cpp
 *
 * Host test for lps35hw<TTransport>. No hardware, HAL or RTOS needed:
 *
 *   g++ -std=c++17 -Wall -Wextra -I../inc -Istub \
 *       -I<safeTransport>/itransport/inc -I<safeTransport>/isensor/inc \
 *       lps35hw_test.cpp <safeTransport>/itransport/src/BusTransport.cpp \
 *       <safeTransport>/itransport/src/I2CTransport.cpp \
 *       <safeTransport>/itransport/src/SPITransport.cpp -o lps35hw_test
 */

#include <cstdio>
#include <cmath>
#include <vector>
#include <utility>
#include "I2CTransport.h"
#include "SPITransport.h"
#include "xlps35hw.h"	// pulls in lps35hw.h. stub/cmsis_os2.h stands in for the RTOS

using namespace LPS35HW;

uint32_t g_slept = 0;						// set by the stub osDelay()
std::vector<uint32_t> g_delays;

// ---- simulated LPS35HW ----
struct MockChip {
	uint8_t reg[256] = {0};
	uint32_t now = 0;						// the test's clock
	bool present = true;
	bool stuck = false;						// transfers never complete
	bool refuse = false;					// transport cannot start a transfer
	bool oneShotStuck = false;
	uint32_t bootUntil = 0;
	uint32_t convEnd = 0;
	int resets = 0, oneShots = 0;
	std::vector<std::pair<uint8_t,uint8_t>> writes;

	MockChip() { powerOn(); setPressure(1013.25f); setTemp(25.12f); }
	void powerOn() {
		for (int r = 0x0B; r <= 0x1A; ++r) reg[r] = 0;
		reg[0x0F] = 0xB1;					// who_am_i
		reg[0x11] = 0x10;					// ctrl_reg2: if_add_inc set
	}
	void setPressure(float hPa) { int32_t raw = (int32_t)lroundf(hPa * 4096.0f); reg[0x28] = raw & 0xFF; reg[0x29] = (raw >> 8) & 0xFF; reg[0x2A] = (raw >> 16) & 0xFF; }
	void setTemp(float c) { int16_t raw = (int16_t)lroundf(c * 100.0f); reg[0x2B] = raw & 0xFF; reg[0x2C] = (raw >> 8) & 0xFF; }
	void write(uint8_t r, uint8_t v) {
		writes.push_back({r, v});
		if (r == 0x11) {
			if (v & 0x04) { ++resets; powerOn(); bootUntil = now + 3; return; }	// swreset wipes every register
			if (v & 0x01) { ++oneShots; convEnd = now + 40; }
			reg[r] = v & ~0x05;				// swreset and one_shot clear themselves
			return;
		}
		reg[r] = v;
	}
	uint8_t read(uint8_t r) const {
		if (r == 0x25) return ((int32_t)(now - bootUntil) < 0) ? 0x80 : 0x00;
		if (r == 0x11) return reg[r] | ((oneShotStuck || (int32_t)(now - convEnd) < 0) ? 0x01 : 0x00);
		return reg[r];
	}
	int count(uint8_t r, uint8_t v) const { int n = 0; for (auto& w : writes) if (w.first == r && w.second == v) ++n; return n; }
	int countReg(uint8_t r) const { int n = 0; for (auto& w : writes) if (w.first == r) ++n; return n; }
};

// ---- transport 1: a bare ISensorTransport whose transfers take time ----
class MockBus : public ISensorTransport {
public:
	explicit MockBus(MockChip& c) : chip(c) {}
	bool writeReg(uint8_t r, uint8_t v) override { return writeRegs(r, &v, 1); }
	bool writeRegs(uint8_t r, const uint8_t* buf, uint8_t len) override {
		if (busy || chip.refuse) return false;
		failed = !chip.present;
		if (chip.present) for (uint8_t i = 0; i < len; ++i) chip.write(r + i, buf[i]);
		busy = true; polls = 1; isRead = false;
		return true;
	}
	bool readRegs(uint8_t r, uint8_t* buf, uint8_t len) override {
		if (busy || chip.refuse) return false;
		failed = !chip.present;
		busy = true; polls = 1; isRead = true; rr = r; dest = buf; n = len;
		return true;
	}
	bool writeBytes(const uint8_t*, uint8_t) override { return false; }	// not used by a register-addressed chip
	bool readBytes(uint8_t*, uint8_t) override { return false; }
	bool isBusy() const override {
		if (!busy) return false;
		if (chip.stuck) return true;
		if (polls > 0) { --polls; return true; }
		if (isRead && !failed) for (uint8_t i = 0; i < n; ++i) dest[i] = chip.read(rr + i);
		busy = false;
		return false;
	}
	bool lastOpFailed() const override { return failed; }
	bool checkDevice() override { return chip.present; }
private:
	MockChip& chip;
	mutable bool busy = false; mutable int polls = 0;
	bool failed = false, isRead = false; uint8_t rr = 0, n = 0; uint8_t* dest = nullptr;
};

// ---- transport 2: itransport's real I2CTransport, with the HAL calls simulated ----
static int g_i2cBus;	// stands in for &hi2c1
class LoopI2C : public I2CTransport {
public:
	explicit LoopI2C(MockChip& c) : I2CTransport(&g_i2cBus, LPS35HW_ADDR, nullptr), chip(c) {}
protected:
	bool halMemWrite(uint8_t reg, uint8_t* p, uint16_t size) override {
		if (chip.present) for (uint16_t i = 0; i < size; ++i) chip.write(reg + i, p[i]);
		BusTransport::onTransferComplete(&g_i2cBus, !chip.present);	// what the I2C interrupt does
		return true;
	}
	bool halMemRead(uint8_t reg, uint8_t* p, uint16_t size) override {
		if (chip.present) for (uint16_t i = 0; i < size; ++i) p[i] = chip.read(reg + i);
		BusTransport::onTransferComplete(&g_i2cBus, !chip.present);
		return true;
	}
	bool halMasterTransmit(uint8_t*, uint16_t) override { return false; }
	bool halMasterReceive(uint8_t*, uint16_t) override { return false; }
	bool halIsDeviceReady(uint32_t, uint32_t) override { return chip.present; }
private:
	MockChip& chip;
};

// ---- transport 3: itransport's real SPITransport, with the HAL calls simulated ----
static int g_spiBus;	// stands in for &hspi1
class LoopSPI : public SPITransport {
public:
	explicit LoopSPI(MockChip& c) : SPITransport(&g_spiBus, nullptr, 0, nullptr), chip(c) {}
protected:
	bool halTransmit(uint8_t* tx, uint16_t len) override {			// write: bit 7 of the address clear
		if (chip.present) for (uint16_t i = 1; i < len; ++i) chip.write(tx[0] + (i - 1), tx[i]);
		BusTransport::onTransferComplete(&g_spiBus, false);			// what the SPI interrupt does
		return true;
	}
	bool halTransmitReceive(uint8_t* tx, uint8_t* rx, uint16_t len) override {	// read: bit 7 set
		rx[0] = 0xFF;
		for (uint16_t i = 1; i < len; ++i) rx[i] = chip.present ? chip.read((tx[0] & 0x7F) + (i - 1)) : 0xFF;
		BusTransport::onTransferComplete(&g_spiBus, false);
		return true;
	}
	void halCsLow() override {}
	void halCsHigh() override {}
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
int run(TSensor& s, MockChip& chip, uint32_t start, uint32_t ms, float* t = nullptr, float* p = nullptr) {
	int readings = 0;
	for (uint32_t i = 0; i < ms; ++i) {
		chip.now = start + i;
		s.main(chip.now);
		if (s.newData()) {
			float tt, pp; s.tempPressure(&tt, &pp);
			if (t) *t = tt;
			if (p) *p = pp;
			++readings;
		}
	}
	return readings;
}

static lps35hw_param_t makeParam() {
	lps35hw_param_t p = {};
	p.odr = lps35hw_odr_0hz;		// one-shot
	p.en_lpfp = true;
	p.lpfp_cfg = true;
	p.drdy = true;
	p.fifo_mode = stream_mode;
	p.fifo_threshold = 5;
	return p;
}

template <typename TTransport>
void normalRun(const char* name, bool spi) {
	std::printf("%s\n", name);
	MockChip chip;
	lps35hw<TTransport> sensor(makeParam(), chip);
	float t = 0, p = 0; sensor.tempPressure(&t, &p);
	check(std::isnan(t) && std::isnan(p), "NAN before the first reading");
	int n = run(sensor, chip, 0, 3500, &t, &p);
	std::printf("        T = %.2f C   P = %.2f hPa   (%d readings in 3.5 s)\n", t, p, n);
	check(std::fabs(p - 1013.25f) < 0.001f && std::fabs(t - 25.12f) < 0.001f, "pressure 1013.25 hPa and temperature 25.12 C");
	check(n == 3, "a reading about every 1.1 s: the period, then the measurement");
	const uint8_t dis = spi ? 0x08 : 0x00;	// i2c_dis
	check(chip.resets == 1 && chip.count(0x11, 0x14 | dis) == 1, "one software reset, and only one");
	check(chip.reg[0x10] == 0x0E, "ctrl_reg1 still holds low-pass on, low-pass config, block data update (0x0E)");
	check(chip.count(0x11, 0x10 | dis) == 1, spi ? "ctrl_reg2 switches the chip's I2C off, as this is SPI (0x18)" : "ctrl_reg2 leaves the chip's I2C on, as this is I2C (0x10)");
	check(chip.reg[0x12] == 0x04, "ctrl_reg3 = data ready on the interrupt pin (0x04)");
	check(chip.reg[0x14] == 0x45, "fifo_ctrl = stream mode in bits 7:5, watermark 5 in bits 4:0 (0x45)");
	check(chip.oneShots == n && chip.count(0x11, 0x11 | dis) == n, "one one-shot trigger per reading");
}

int main() {
	normalRun<MockBus>("lps35hw<MockBus>: bare ISensorTransport, asynchronous", false);
	normalRun<LoopI2C>("lps35hw<LoopI2C>: itransport's I2CTransport", false);
	normalRun<LoopSPI>("lps35hw<LoopSPI>: itransport's SPITransport", true);

	std::printf("values below zero\n");
	{
		MockChip chip; chip.setTemp(-5.25f); chip.setPressure(-12.5f);	// pressure can read negative with a reference set
		lps35hw<LoopI2C> sensor(makeParam(), chip);
		float t = NAN, p = NAN; run(sensor, chip, 0, 1500, &t, &p);
		check(std::fabs(t - (-5.25f)) < 0.001f, "temperature -5.25 C");
		check(std::fabs(p - (-12.5f)) < 0.001f, "24 bit pressure is sign extended");
	}

	std::printf("startMeasurement(): a measurement ahead of the period\n");
	{
		MockChip chip;
		lps35hw<LoopI2C> sensor(makeParam(), chip);
		run(sensor, chip, 0, 1500);						// first periodic reading done at about 1.25 s
		const int before = chip.oneShots;
		sensor.startMeasurement();
		const int soon = run(sensor, chip, 1500, 300);
		check(soon == 1 && chip.oneShots == before + 1, "taken up within 300 ms instead of waiting out the second");
		const int later = run(sensor, chip, 1800, 3000);
		check(later >= 2 && later <= 3, "one request gives one extra measurement, then back to one a second");
	}

	std::printf("chip measuring by itself (odr above 0)\n");
	{
		MockChip chip;
		lps35hw_param_t param = makeParam(); param.odr = lps35hw_odr_10hz;
		lps35hw<LoopI2C> sensor(param, chip);
		float t = NAN, p = NAN;
		const int n = run(sensor, chip, 0, 3500, &t, &p);
		check(n == 3 && chip.oneShots == 0, "latest result read each period, with no one-shot trigger");
		check(chip.reg[0x10] == 0x2E, "ctrl_reg1 carries the 10 Hz rate (0x2E)");
	}

	std::printf("device absent, then plugged in\n");
	{
		MockChip chip; chip.present = false;
		lps35hw<LoopI2C> sensor(makeParam(), chip);
		check(run(sensor, chip, 0, 2000) == 0, "no readings while absent");
		chip.present = true;
		check(run(sensor, chip, 2000, 3000) >= 1, "recovers by itself once the device answers");
	}

	std::printf("device absent on SPI, where there is no acknowledge\n");
	{
		MockChip chip; chip.present = false;
		lps35hw<LoopSPI> sensor(makeParam(), chip);
		check(run(sensor, chip, 0, 2000) == 0, "who_am_i check rejects a bus that reads 0xFF");
	}

	std::printf("wrong chip at the address\n");
	{
		MockChip chip; chip.reg[0x0F] = 0xB4;
		lps35hw<MockBus> sensor(makeParam(), chip);
		check(run(sensor, chip, 0, 3000) == 0 && chip.resets == 0, "no readings, and it is never reset or configured");
	}

	std::printf("transfer that never completes\n");
	{
		MockChip chip;
		lps35hw<MockBus> sensor(makeParam(), chip);
		run(sensor, chip, 0, 1100);
		chip.stuck = true;
		run(sensor, chip, 1100, 2000);
		check(sensor.state() == lps35hw_err_state, "gives up after the bus timeout");
		chip.stuck = false;
		check(run(sensor, chip, 3100, 3000) >= 1, "recovers when the bus comes back");
	}

	std::printf("transport that cannot start a transfer\n");
	{
		MockChip chip;
		lps35hw<MockBus> sensor(makeParam(), chip);
		run(sensor, chip, 0, 500);
		size_t before = chip.writes.size();
		chip.refuse = true;
		check(run(sensor, chip, 500, 2500) == 0 && chip.writes.size() == before, "nothing reaches the bus");
		chip.refuse = false;
		check(run(sensor, chip, 3000, 3000) >= 1, "recovers when the bus is free");
	}

	std::printf("one_shot bit stuck high\n");
	{
		MockChip chip;
		lps35hw<MockBus> sensor(makeParam(), chip);
		run(sensor, chip, 0, 500);
		chip.oneShotStuck = true;
		check(run(sensor, chip, 500, 4000) == 0, "no reading reported");
		check(chip.resets >= 2, "times out and re-initialises instead of waiting for ever");
	}

	std::printf("tick counter rollover\n");
	{
		MockChip chip;
		lps35hw<LoopI2C> sensor(makeParam(), chip);
		const uint32_t start = 0xFFFFFFFFu - 2500;	// wraps 2.5 s into the run
		check(run(sensor, chip, start, 6000) == 5, "readings keep their rhythm across the wrap");
	}

	std::printf("xlps35hw: sleep times under an OS\n");
	{
		MockChip chip;
		xlps35hw<MockBus> sensor(makeParam(), chip);
		// Like a thread: main(), then advance the clock by however long
		// main() slept, plus the one tick the loop gives up every cycle.
		uint32_t now = 0; int calls = 0, readings = 0; uint32_t longest = 0;
		g_delays.clear();
		while (now < 10000) {
			chip.now = now; g_slept = 0;
			sensor.main(now); ++calls;
			if (sensor.newData()) { float a, b; sensor.tempPressure(&a, &b); ++readings; }
			now += g_slept + 1;
		}
		for (uint32_t d : g_delays) if (d > longest) longest = d;
		std::printf("        %d readings in 10 s from %d calls to main(), longest sleep %u ms\n", readings, calls, (unsigned)longest);
		check(readings >= 8, "still measures on schedule");
		check(longest == 100, "never sleeps longer than 100 ms, so a startMeasurement() request is seen promptly");
		check(calls < 300, "wakes only when there is something to do");
	}

	std::printf("\n%s\n", g_failures ? "FAILED" : "all passed");
	return g_failures ? 1 : 0;
}
