/*
 * mpl3115a2_test.cpp
 *
 * Host test for mpl3115a2<TTransport>. No hardware, HAL or RTOS needed:
 *
 *   g++ -std=c++17 -Wall -Wextra -I../inc -Istub \
 *       -I<safeTransport>/itransport/inc -I<safeTransport>/isensor/inc \
 *       mpl3115a2_test.cpp <safeTransport>/itransport/src/BusTransport.cpp \
 *       <safeTransport>/itransport/src/I2CTransport.cpp -o mpl3115a2_test
 */

#include <cstdio>
#include <cmath>
#include <vector>
#include <utility>
#include "I2CTransport.h"
#include "xmpl3115a2.h"	// pulls in mpl3115a2.h. stub/cmsis_os2.h stands in for the RTOS

uint32_t g_slept = 0;						// set by the stub osDelay()
std::vector<uint32_t> g_delays;

using mpl3115a2::mpl3115a2_param_t;

// ---- simulated MPL3115A2 ----
struct MockChip {
	uint8_t reg[256] = {0};
	uint32_t now = 0;						// the test's clock
	bool present = true;
	bool stuck = false;						// transfers never complete
	bool refuse = false;					// transport cannot start a transfer
	bool nackReset = true;					// resets before acknowledging the reset command
	bool neverReady = false;				// conversion never finishes
	bool resetting = false;
	uint32_t resetUntil = 0;				// while resetting, silent on the bus until then
	uint32_t convEnd = 0; bool converting = false;
	uint32_t conversionTime = 60;
	int resets = 0, oneShots = 0;
	std::vector<std::pair<uint8_t,uint8_t>> writes;

	MockChip() { reg[0x0C] = 0xC4; setPressure(1013.25f); setTemp(25.5f); }
	void setPressure(float hPa) { uint32_t raw = (uint32_t)lroundf(hPa * 6400.0f); reg[0x01] = raw >> 16; reg[0x02] = (raw >> 8) & 0xFF; reg[0x03] = raw & 0xF0; }
	void setAltitude(float m) { int32_t raw = (int32_t)lroundf(m * 65536.0f); reg[0x01] = (raw >> 24) & 0xFF; reg[0x02] = (raw >> 16) & 0xFF; reg[0x03] = (raw >> 8) & 0xF0; }
	void setTemp(float c) { int16_t raw = (int16_t)lroundf(c * 256.0f); reg[0x04] = (raw >> 8) & 0xFF; reg[0x05] = raw & 0xF0; }

	bool silent() {
		if (resetting && (int32_t)(now - resetUntil) >= 0) resetting = false;
		return !present || resetting;
	}

	// Both return false when the chip does not acknowledge.
	bool write(uint8_t r, uint8_t v) {
		if (silent()) return false;
		writes.push_back({r, v});
		if (r == 0x26 && (v & 0x04)) {		// RST
			++resets;
			for (int i = 0x26; i <= 0x2A; ++i) reg[i] = 0;
			reg[0x13] = 0;
			resetting = true; resetUntil = now + 3;
			return !nackReset;
		}
		if (r == 0x26 && (v & 0x02)) { ++oneShots; converting = true; convEnd = now + conversionTime; reg[0x00] = 0; }
		reg[r] = v;
		return true;
	}
	bool read(uint8_t r, uint8_t* out) {
		if (silent()) return false;
		const bool done = converting && !neverReady && (int32_t)(now - convEnd) >= 0;
		if (done) { converting = false; reg[0x00] = 0x0E; reg[0x26] &= ~0x02; }	// PTDR, PDR, TDR set; OST clears
		*out = reg[r];
		if (r == 0x01) reg[0x00] = 0;		// reading the data clears the ready flags
		return true;
	}
	int indexOf(uint8_t r, uint8_t v) const {
		for (size_t i = 0; i < writes.size(); ++i) if (writes[i].first == r && writes[i].second == v) return (int)i;
		return -1;
	}
	int countReg(uint8_t r) const { int n = 0; for (auto& w : writes) if (w.first == r) ++n; return n; }
};

// ---- transport 1: a bare ISensorTransport whose transfers take time ----
class MockBus : public ISensorTransport {
public:
	explicit MockBus(MockChip& c) : chip(c) {}
	bool writeReg(uint8_t r, uint8_t v) override { return writeRegs(r, &v, 1); }
	bool writeRegs(uint8_t r, const uint8_t* buf, uint8_t len) override {
		if (busy || chip.refuse) return false;
		failed = false;
		for (uint8_t i = 0; i < len; ++i) if (!chip.write(r + i, buf[i])) failed = true;
		busy = true; polls = 1; isRead = false;
		return true;
	}
	bool readRegs(uint8_t r, uint8_t* buf, uint8_t len) override {
		if (busy || chip.refuse) return false;
		failed = false;
		busy = true; polls = 1; isRead = true; rr = r; dest = buf; n = len;
		return true;
	}
	bool writeBytes(const uint8_t*, uint8_t) override { return false; }	// not used by a register-addressed chip
	bool readBytes(uint8_t*, uint8_t) override { return false; }
	bool isBusy() const override {
		if (!busy) return false;
		if (chip.stuck) return true;
		if (polls > 0) { --polls; return true; }
		if (isRead) for (uint8_t i = 0; i < n; ++i) if (!chip.read(rr + i, &dest[i])) failed = true;
		busy = false;
		return false;
	}
	bool lastOpFailed() const override { return failed; }
	bool checkDevice() override { return chip.present; }
private:
	MockChip& chip;
	mutable bool busy = false; mutable int polls = 0; mutable bool failed = false;
	bool isRead = false; uint8_t rr = 0, n = 0; uint8_t* dest = nullptr;
};

// ---- transport 2: itransport's real I2CTransport, with the HAL calls simulated ----
static int g_i2cBus;	// stands in for &hi2c1
class LoopI2C : public I2CTransport {
public:
	explicit LoopI2C(MockChip& c) : I2CTransport(&g_i2cBus, mpl3115a2::MPL3115A2_DEV_ADDRESS, nullptr), chip(c) {}
protected:
	bool halMemWrite(uint8_t reg, uint8_t* p, uint16_t size) override {
		bool ok = true;
		for (uint16_t i = 0; i < size; ++i) if (!chip.write(reg + i, p[i])) ok = false;
		BusTransport::onTransferComplete(&g_i2cBus, !ok);	// what the I2C interrupt, or error interrupt, does
		return true;
	}
	bool halMemRead(uint8_t reg, uint8_t* p, uint16_t size) override {
		bool ok = true;
		for (uint16_t i = 0; i < size; ++i) if (!chip.read(reg + i, &p[i])) ok = false;
		BusTransport::onTransferComplete(&g_i2cBus, !ok);
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

struct Reading { float t = NAN, p = NAN, alt = NAN; };

// Runs main() once per ms, like a bare metal loop. Returns readings seen.
template <typename TSensor>
int run(TSensor& s, MockChip& chip, uint32_t start, uint32_t ms, Reading* last = nullptr) {
	int readings = 0;
	for (uint32_t i = 0; i < ms; ++i) {
		chip.now = start + i;
		s.main(chip.now);
		if (s.newData()) {
			Reading r; r.t = s.getTemp(); r.p = s.getPressure(); r.alt = s.getAltitude();
			if (last) *last = r;
			++readings;
		}
	}
	return readings;
}

static const mpl3115a2_param_t kBarometer = {mpl3115a2::mpl3115a2_barometer_mode, mpl3115a2::MPL3115A2_CTRL_REG1_OS8};
static const mpl3115a2_param_t kAltimeter = {mpl3115a2::mpl3115a2_altimeter_mode, mpl3115a2::MPL3115A2_CTRL_REG1_OS8};

template <typename TTransport>
void normalRun(const char* name) {
	std::printf("%s\n", name);
	MockChip chip;
	mpl3115a2::mpl3115a2<TTransport> sensor(kBarometer, chip);
	check(std::isnan(sensor.getTemp()) && std::isnan(sensor.getPressure()), "NAN before the first reading");
	Reading r;
	int n = run(sensor, chip, 0, 11000, &r);
	std::printf("        T = %.2f C   P = %.2f hPa   (%d readings in 11 s)\n", r.t, r.p, n);
	check(std::fabs(r.p - 1013.25f) < 0.01f && std::fabs(r.t - 25.5f) < 0.001f, "pressure 1013.25 hPa and temperature 25.5 C");
	check(std::isnan(r.alt), "no altitude claimed in barometer mode");
	check(n == 2, "a reading every 5 s");
	check(chip.resets == 1, "one software reset, although the chip did not acknowledge it");
	check(chip.reg[0x13] == 0x07, "pt_data_cfg = all data-ready event flags on (0x07)");
	check(chip.indexOf(0x26, 0x18) >= 0, "ctrl_reg1 = barometer, oversampling 8 (0x18)");
	check(chip.oneShots == n && chip.indexOf(0x26, 0x1A) >= 0, "one one-shot trigger per reading (0x1A)");
	check(chip.countReg(0x29) == 0 && chip.countReg(0x2A) == 0, "interrupt registers left alone when SetIRQ() was not called");
}

int main() {
	normalRun<MockBus>("mpl3115a2<MockBus>: bare ISensorTransport, asynchronous");
	normalRun<LoopI2C>("mpl3115a2<LoopI2C>: itransport's I2CTransport");

	std::printf("temperature below zero\n");
	{
		MockChip chip; chip.setTemp(-10.25f);
		mpl3115a2::mpl3115a2<LoopI2C> sensor(kBarometer, chip);
		Reading r; run(sensor, chip, 0, 6000, &r);
		check(std::fabs(r.t - (-10.25f)) < 0.001f, "-10.25 C");
	}

	std::printf("altimeter mode\n");
	{
		MockChip chip; chip.setAltitude(123.5f);
		mpl3115a2::mpl3115a2<LoopI2C> sensor(kAltimeter, chip);
		Reading r; run(sensor, chip, 0, 6000, &r);
		check(std::fabs(r.alt - 123.5f) < 0.001f, "altitude 123.5 m");
		check(std::isnan(r.p), "no pressure claimed in altimeter mode");
		check(chip.indexOf(0x26, 0x98) >= 0, "ctrl_reg1 = altimeter, oversampling 8 (0x98)");
		chip.setAltitude(-50.25f);
		run(sensor, chip, 6000, 5500, &r);
		check(std::fabs(r.alt - (-50.25f)) < 0.001f, "below sea level: -50.25 m");
	}

	std::printf("SetIRQ()\n");
	{
		MockChip chip;
		mpl3115a2::mpl3115a2<LoopI2C> sensor(kBarometer, chip);
		mpl3115a2::irq_en_param_t en; en.reg = 0; en.bit.INT_EN_DRDY = 1;
		mpl3115a2::irq_cfg_param_t cfg; cfg.reg = 0; cfg.bit.INT_CFG_DRDY = 1;
		sensor.SetIRQ(en, cfg);
		run(sensor, chip, 0, 500);
		check(chip.reg[0x29] == 0x80 && chip.reg[0x2A] == 0x80, "before start up: data-ready enabled (ctrl_reg4) and routed (ctrl_reg5)");
		check(chip.indexOf(0x2A, 0x80) < chip.indexOf(0x29, 0x80), "routing written before the enable");
		en.bit.INT_EN_PTH = 1;
		sensor.SetIRQ(en, cfg);
		run(sensor, chip, 500, 100);
		check(chip.reg[0x29] == 0x88, "while running: the change reaches the chip without a restart");
		check(chip.resets == 1, "and without another reset");
	}

	std::printf("slow conversion (high oversampling)\n");
	{
		MockChip chip; chip.conversionTime = 512;
		mpl3115a2::mpl3115a2<LoopI2C> sensor(kBarometer, chip);
		check(run(sensor, chip, 0, 6000) == 1 && chip.resets == 1, "status is polled until the result is ready");
	}

	std::printf("conversion that never finishes\n");
	{
		MockChip chip; chip.neverReady = true;
		mpl3115a2::mpl3115a2<MockBus> sensor(kBarometer, chip);
		check(run(sensor, chip, 0, 8000) == 0, "no reading reported");
		check(chip.resets >= 2, "times out and re-initialises instead of waiting for ever");
	}

	std::printf("device absent, then plugged in\n");
	{
		MockChip chip; chip.present = false;
		mpl3115a2::mpl3115a2<LoopI2C> sensor(kBarometer, chip);
		check(run(sensor, chip, 0, 3000) == 0, "no readings while absent");
		chip.present = true;
		check(run(sensor, chip, 3000, 6000) >= 1, "recovers by itself once the device answers");
	}

	std::printf("wrong chip at the address\n");
	{
		MockChip chip; chip.reg[0x0C] = 0x0E;
		mpl3115a2::mpl3115a2<MockBus> sensor(kBarometer, chip);
		check(run(sensor, chip, 0, 3000) == 0 && chip.resets == 0, "no readings, and it is never reset or configured");
		check(sensor.state() == mpl3115a2::mpl3115a2_error_state || sensor.state() == mpl3115a2::mpl3115a2_init_state
			  || sensor.state() == mpl3115a2::mpl3115a2_whoami_state || sensor.state() == mpl3115a2::mpl3115a2_wait_whoami_state,
			  "keeps retrying through the error state");
	}

	std::printf("transfer that never completes\n");
	{
		MockChip chip;
		mpl3115a2::mpl3115a2<MockBus> sensor(kBarometer, chip);
		run(sensor, chip, 0, 5050);
		chip.stuck = true;
		run(sensor, chip, 5050, 2000);
		check(sensor.state() == mpl3115a2::mpl3115a2_error_state, "gives up after the bus timeout");
		chip.stuck = false;
		check(run(sensor, chip, 7050, 6000) >= 1, "recovers when the bus comes back");
	}

	std::printf("transport that cannot start a transfer\n");
	{
		MockChip chip;
		mpl3115a2::mpl3115a2<MockBus> sensor(kBarometer, chip);
		run(sensor, chip, 0, 500);
		size_t before = chip.writes.size();
		chip.refuse = true;
		check(run(sensor, chip, 500, 6000) == 0 && chip.writes.size() == before, "nothing reaches the bus");
		chip.refuse = false;
		check(run(sensor, chip, 6500, 6000) >= 1, "recovers when the bus is free");
	}

	std::printf("tick counter rollover\n");
	{
		MockChip chip;
		mpl3115a2::mpl3115a2<LoopI2C> sensor(kBarometer, chip);
		const uint32_t start = 0xFFFFFFFFu - 7500;	// wraps 7.5 s into the run
		check(run(sensor, chip, start, 16000) == 3, "readings keep their 5 s rhythm across the wrap");
	}

	std::printf("xmpl3115a2: sleep times under an OS\n");
	{
		MockChip chip;
		mpl3115a2::xmpl3115a2<MockBus> sensor(kBarometer, chip);
		// Like a thread: main(), then advance the clock by however long
		// main() slept, plus the one tick the loop gives up every cycle.
		uint32_t now = 0; int calls = 0, readings = 0; uint32_t longest = 0;
		g_delays.clear();
		while (now < 30000) {
			chip.now = now; g_slept = 0;
			sensor.main(now); ++calls;
			if (sensor.newData()) { (void)sensor.getTemp(); ++readings; }
			now += g_slept + 1;
		}
		for (uint32_t d : g_delays) if (d > longest) longest = d;
		std::printf("        %d readings in 30 s from %d calls to main(), longest sleep %u ms\n", readings, calls, (unsigned)longest);
		check(readings == 5, "still measures on schedule");
		check(longest >= 4990, "sleeps the whole 5 s between measurements");
		check(calls < 200, "wakes only when there is something to do");
	}

	std::printf("\n%s\n", g_failures ? "FAILED" : "all passed");
	return g_failures ? 1 : 0;
}
