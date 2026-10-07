/*
 * mmc56x3_test.cpp
 *
 * Host test for mmc56x3<TTransport>. No hardware, HAL or RTOS needed.
 * IT is the itransport folder (iTransport/itransport):
 *
 *   g++ -std=c++17 -Wall -Wextra -I../inc -Istub \
 *       -I$IT/inc -I<isensor>/inc \
 *       mmc56x3_test.cpp $IT/src/BusTransport.cpp $IT/src/I2CTransport.cpp -o mmc56x3_test
 */

#include <cstdio>
#include <cmath>
#include <vector>
#include <utility>
#include "I2CTransport.h"
#include "xmmc56x3.h"	// pulls in mmc56x3.h. stub/cmsis_os2.h stands in for the RTOS

using namespace MMC56X3;

uint32_t g_slept = 0;						// set by the stub osDelay()
std::vector<uint32_t> g_delays;

// ---- simulated MMC5603 ----
// The control registers are write-only: reading them back gives 0, so
// a driver that relied on reading them would go wrong here as on the
// real chip.
struct MockChip {
	uint8_t reg[256] = {0};
	uint32_t now = 0;						// the test's clock
	bool present = true;
	bool stuck = false;						// transfers never complete
	bool refuse = false;					// transport cannot start a transfer
	bool neverDone = false;					// measurements never finish
	uint8_t productId = 0x10;
	uint32_t mDoneAt = 0, tDoneAt = 0;
	bool mPending = false, tPending = false;
	int resets = 0, sets = 0, resetPulses = 0, mTriggers = 0, tTriggers = 0;
	uint8_t ctrl0 = 0, ctrl1 = 0, ctrl2 = 0, odr = 0;	// last values written
	std::vector<std::pair<uint8_t,uint8_t>> writes;

	MockChip() { setField(12.5f, -3.75f, 40.0f); setTemp(25.2f); }
	void setField(float x, float y, float z) {
		const float f[3] = {x, y, z};
		for (int i = 0; i < 3; ++i) {
			const uint32_t raw = (uint32_t)((int32_t)lroundf(f[i] / 0.00625f) + (1 << 19));
			reg[2*i] = (raw >> 12) & 0xFF;
			reg[2*i+1] = (raw >> 4) & 0xFF;
			reg[6+i] = (uint8_t)((raw & 0x0F) << 4);
		}
	}
	void setTemp(float c) { reg[0x09] = (uint8_t)lroundf((c + 75.0f) / 0.8f); }
	void write(uint8_t r, uint8_t v) {
		writes.push_back({r, v});
		switch (r) {
		case 0x1A: odr = v; break;
		case 0x1B:
			ctrl0 = v;
			if (v & 0x01) { ++mTriggers; mPending = true; mDoneAt = now + 5; }
			if (v & 0x02) { ++tTriggers; tPending = true; tDoneAt = now + 2; }
			if (v & 0x08) ++sets;
			if (v & 0x10) ++resetPulses;
			break;
		case 0x1C: if (v & 0x80) ++resets; else ctrl1 = v; break;
		case 0x1D: ctrl2 = v; break;
		default: break;
		}
	}
	uint8_t read(uint8_t r) const {
		if (r == 0x39) return productId;
		if (r == 0x18) {
			uint8_t s = 0;
			if (!neverDone && !(mPending && (int32_t)(now - mDoneAt) < 0)) s |= 0x40;
			if (!neverDone && !(tPending && (int32_t)(now - tDoneAt) < 0)) s |= 0x80;
			return s;
		}
		if (r >= 0x1A && r <= 0x1D) return 0;	// write-only
		return reg[r];
	}
	int count(uint8_t r, uint8_t v) const { int n = 0; for (auto& w : writes) if (w.first == r && w.second == v) ++n; return n; }
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
	explicit LoopI2C(MockChip& c) : I2CTransport(&g_i2cBus, MMC56X3_I2C_ADDR, nullptr), chip(c) {}
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

// ---- harness ----
static int g_failures = 0;
static void check(bool ok, const char* what) {
	std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok) ++g_failures;
}

struct Reading { float x = NAN, y = NAN, z = NAN, t = NAN; };

// Runs main() once per ms, like a bare metal loop. Returns readings seen.
template <typename TSensor>
int run(TSensor& s, MockChip& chip, uint32_t start, uint32_t ms, Reading* last = nullptr) {
	int readings = 0;
	for (uint32_t i = 0; i < ms; ++i) {
		chip.now = start + i;
		s.main(chip.now);
		if (s.newData()) {
			Reading r; s.getTemperature(&r.t); s.getField(&r.x, &r.y, &r.z);
			if (last) *last = r;
			++readings;
		}
	}
	return readings;
}

static mmc56x3_param_t oneShot() {
	mmc56x3_param_t p = {};
	p.mode = mmc56x3_one_shot;
	p.bandwidth = mmc56x3_bw_6_6ms;
	p.odr = 0;
	p.auto_sr = true;
	p.read_temperature = true;
	p.period_ms = 1000;
	return p;
}

static bool near(float a, float b) { return std::fabs(a - b) < 0.004f; }

template <typename TTransport>
void normalRun(const char* name) {
	std::printf("%s\n", name);
	MockChip chip;
	mmc56x3<TTransport> sensor(oneShot(), chip);
	Reading r; sensor.getField(&r.x, &r.y, &r.z); sensor.getTemperature(&r.t);
	check(std::isnan(r.x) && std::isnan(r.y) && std::isnan(r.z) && std::isnan(r.t), "NAN before the first reading");
	const int n = run(sensor, chip, 0, 3500, &r);
	std::printf("        x = %.4f  y = %.4f  z = %.4f uT   T = %.1f C   (%d readings in 3.5 s)\n", r.x, r.y, r.z, r.t, n);
	check(near(r.x, 12.5f) && near(r.y, -3.75f) && near(r.z, 40.0f), "field 12.5, -3.75, 40 uT");
	check(std::fabs(r.t - 25.2f) < 0.41f, "temperature 25.2 C, to the 0.8 C step");
	check(n == 3, "a reading every second");
	check(chip.resets == 1 && chip.sets == 1 && chip.resetPulses == 1, "one software reset, one set pulse and one reset pulse");
	check(chip.ctrl1 == 0x00, "internal control 1 = bandwidth 0 (6.6 ms)");
	check(chip.mTriggers == n && chip.tTriggers == n, "one field and one temperature trigger per reading");
	check(chip.count(0x1B, 0x21) == n && chip.count(0x1B, 0x22) == n, "triggers carry auto set/reset (0x21, 0x22)");
	check(chip.count(0x1D, 0x10) == 0 && chip.count(0x1A, 0) == 0, "continuous mode is never switched on");
}

int main() {
	normalRun<MockBus>("mmc56x3<MockBus>: bare ISensorTransport, asynchronous");
	normalRun<LoopI2C>("mmc56x3<LoopI2C>: itransport's I2CTransport");

	std::printf("full scale field, both signs\n");
	{
		MockChip chip; chip.setField(-3000.0f, 3000.0f, 0.0f);
		mmc56x3<LoopI2C> sensor(oneShot(), chip);
		Reading r; run(sensor, chip, 0, 1500, &r);
		check(near(r.x, -3000.0f) && near(r.y, 3000.0f) && near(r.z, 0.0f), "-3000, 3000 and 0 uT, from all 20 bits");
	}

	std::printf("one-shot without the temperature\n");
	{
		MockChip chip;
		mmc56x3_param_t p = oneShot(); p.read_temperature = false; p.auto_sr = false;
		mmc56x3<LoopI2C> sensor(p, chip);
		Reading r; const int n = run(sensor, chip, 0, 2500, &r);
		check(n == 2 && chip.tTriggers == 0 && std::isnan(r.t), "no temperature trigger, temperature NAN");
		check(chip.count(0x1B, 0x01) == n, "field trigger without auto set/reset (0x01)");
	}

	std::printf("narrow bandwidth\n");
	{
		MockChip chip;
		mmc56x3_param_t p = oneShot(); p.bandwidth = mmc56x3_bw_1_2ms;
		mmc56x3<LoopI2C> sensor(p, chip);
		check(run(sensor, chip, 0, 1500) == 1 && chip.ctrl1 == 0x03, "internal control 1 = 3, and still reads");
	}

	std::printf("continuous mode at 100 a second\n");
	{
		MockChip chip;
		mmc56x3_param_t p = oneShot(); p.mode = mmc56x3_continuous; p.odr = 100;
		mmc56x3<LoopI2C> sensor(p, chip);
		Reading r; const int n = run(sensor, chip, 0, 3500, &r);
		check(n == 3 && chip.mTriggers == 0 && chip.tTriggers == 0, "latest field read each period, with no trigger");
		check(chip.odr == 100 && chip.count(0x1B, 0xA0) == 1 && chip.count(0x1D, 0x10) == 1,
			"data rate 100, then cmm_freq_en with auto set/reset (0xA0), then cmm_en (0x10)");
		size_t iOdr = 0, iFreq = 0, iEn = 0;
		for (size_t i = 0; i < chip.writes.size(); ++i) {
			if (chip.writes[i].first == 0x1A) iOdr = i;
			if (chip.writes[i] == std::make_pair((uint8_t)0x1B, (uint8_t)0xA0)) iFreq = i;
			if (chip.writes[i].first == 0x1D) iEn = i;
		}
		check(iOdr < iFreq && iFreq < iEn, "in the order the datasheet gives");
		check(near(r.x, 12.5f) && std::isnan(r.t), "field read, no temperature in continuous mode");
	}

	std::printf("continuous mode at 1000 a second\n");
	{
		MockChip chip;
		mmc56x3_param_t p = oneShot(); p.mode = mmc56x3_continuous; p.odr = 1000;
		mmc56x3<LoopI2C> sensor(p, chip);
		run(sensor, chip, 0, 500);
		check(chip.odr == 255 && chip.ctrl2 == 0x90, "data rate 255 with hpower (internal control 2 = 0x90)");
	}

	std::printf("startMeasurement(): a reading ahead of the period\n");
	{
		MockChip chip;
		mmc56x3<LoopI2C> sensor(oneShot(), chip);
		run(sensor, chip, 0, 1500);						// first periodic reading at about 1 s
		const int before = chip.mTriggers;
		sensor.startMeasurement();
		const int soon = run(sensor, chip, 1500, 150);
		check(soon == 1 && chip.mTriggers == before + 1, "taken up within 150 ms instead of waiting out the period");
	}

	std::printf("device absent, then plugged in\n");
	{
		MockChip chip; chip.present = false;
		mmc56x3<LoopI2C> sensor(oneShot(), chip);
		check(run(sensor, chip, 0, 2000) == 0, "no readings while absent");
		chip.present = true;
		check(run(sensor, chip, 2000, 3000) >= 1, "recovers by itself once the device answers");
	}

	std::printf("wrong chip at the address\n");
	{
		MockChip chip; chip.productId = 0x11;
		mmc56x3<MockBus> sensor(oneShot(), chip);
		check(run(sensor, chip, 0, 3000) == 0 && chip.resets == 0, "no readings, and it is never reset or configured");
	}

	std::printf("measurement that never finishes\n");
	{
		MockChip chip;
		mmc56x3<MockBus> sensor(oneShot(), chip);
		Reading r; run(sensor, chip, 0, 1500, &r);
		chip.neverDone = true;
		check(run(sensor, chip, 1500, 3000) == 0, "no reading reported");
		float x = 0, y = 0, z = 0; sensor.getField(&x, &y, &z);
		check(std::isnan(x) && chip.resets >= 2, "times out, marks the field NAN and starts again");
		chip.neverDone = false;
		check(run(sensor, chip, 4500, 3000) >= 1, "reads again once measurements finish");
	}

	std::printf("transfer that never completes\n");
	{
		MockChip chip;
		mmc56x3<MockBus> sensor(oneShot(), chip);
		run(sensor, chip, 0, 1100);
		chip.stuck = true;
		run(sensor, chip, 1100, 2000);
		check(sensor.state() == mmc56x3_error_state, "gives up after the bus timeout");
		chip.stuck = false;
		check(run(sensor, chip, 3100, 3000) >= 1, "recovers when the bus comes back");
	}

	std::printf("transport that cannot start a transfer\n");
	{
		MockChip chip;
		mmc56x3<MockBus> sensor(oneShot(), chip);
		run(sensor, chip, 0, 500);
		const size_t before = chip.writes.size();
		chip.refuse = true;
		check(run(sensor, chip, 500, 2500) == 0 && chip.writes.size() == before, "nothing reaches the bus");
		chip.refuse = false;
		check(run(sensor, chip, 3000, 3000) >= 1, "recovers when the bus is free");
	}

	std::printf("tick counter rollover\n");
	{
		MockChip chip;
		mmc56x3<LoopI2C> sensor(oneShot(), chip);
		const uint32_t start = 0xFFFFFFFFu - 2500;	// wraps 2.5 s into the run
		check(run(sensor, chip, start, 6000) == 5, "readings keep their rhythm across the wrap");
	}

	std::printf("xmmc56x3: sleep times under an OS\n");
	{
		MockChip chip;
		xmmc56x3<MockBus> sensor(oneShot(), chip);
		// Like a thread: main(), then advance the clock by however long
		// main() slept, plus the one tick the loop gives up every cycle.
		uint32_t now = 0; int calls = 0, readings = 0; uint32_t longest = 0;
		g_delays.clear();
		while (now < 10000) {
			chip.now = now; g_slept = 0;
			sensor.main(now); ++calls;
			if (sensor.newData()) { float a, b, c; sensor.getField(&a, &b, &c); ++readings; }
			now += g_slept + 1;
		}
		for (uint32_t d : g_delays) if (d > longest) longest = d;
		std::printf("        %d readings in 10 s from %d calls to main(), longest sleep %u ms\n", readings, calls, (unsigned)longest);
		check(readings >= 9, "still measures on schedule");
		check(longest == 100, "never sleeps longer than 100 ms, so a startMeasurement() request is seen promptly");
		check(calls < 400, "wakes only when there is something to do");
	}

	std::printf("\n%s\n", g_failures ? "FAILED" : "all passed");
	return g_failures ? 1 : 0;
}
