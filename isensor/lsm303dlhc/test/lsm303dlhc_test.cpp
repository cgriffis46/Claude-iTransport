/*
 * lsm303dlhc_test.cpp
 *
 * Host test for lsm303dlhc_accel<TTransport> and lsm303dlhc_mag<TTransport>.
 * No hardware, HAL or RTOS needed. IT is the itransport folder
 * (iTransport/itransport):
 *
 *   g++ -std=c++17 -Wall -Wextra -I../inc -Istub \
 *       -I$IT/inc -I<isensor>/inc \
 *       lsm303dlhc_test.cpp $IT/src/BusTransport.cpp $IT/src/I2CTransport.cpp -o lsm303dlhc_test
 */

#include <cstdio>
#include <cmath>
#include <vector>
#include <utility>
#include "I2CTransport.h"
#include "xlsm303dlhc.h"	// pulls in lsm303dlhc.h. stub/cmsis_os2.h stands in for the RTOS

using namespace LSM303DLHC;

uint32_t g_slept = 0;						// set by the stub osDelay()
std::vector<uint32_t> g_delays;

static const float G = 9.80665f;

// ---- what both simulated halves have in common ----
struct MockChip {
	uint8_t reg[256] = {0};
	uint32_t now = 0;						// the test's clock
	bool present = true;
	bool stuck = false;						// transfers never complete
	bool refuse = false;					// transport cannot start a transfer
	std::vector<std::pair<uint8_t,uint8_t>> writes;
	int dataReads = 0;

	virtual ~MockChip() {}
	virtual void write(uint8_t r, uint8_t v) { writes.push_back({r, v}); reg[r] = v; }
	virtual uint8_t read(uint8_t r) { return reg[r]; }
	// One register-addressed read, as the chip sees it.
	virtual void readRun(uint8_t r, uint8_t* p, uint16_t n) { for (uint16_t i = 0; i < n; ++i) p[i] = read(r + i); }
	int count(uint8_t r, uint8_t v) const { int c = 0; for (auto& w : writes) if (w.first == r && w.second == v) ++c; return c; }
};

// ---- simulated accelerometer ----
// Samples at the rate in CTRL_REG1_A, sets ZYXDA when a new one is
// ready, and clears it when the sample is read. Like the real chip it
// only moves on to the next register in a multi-byte read when bit 7
// of the address is set.
struct MockAccel : MockChip {
	bool ignoreWrites = false;				// something that is not an LSM303DLHC
	bool stalled = false;					// stops producing samples
	int16_t raw[3] = {0, 0, 0};				// 12 bit counts
	uint32_t lastSample = 0;
	bool ready = false;

	void setG(float x, float y, float z, float mgPerLsb) {
		const float g[3] = {x, y, z};
		for (int i = 0; i < 3; ++i) raw[i] = (int16_t)lroundf(g[i] * 1000.0f / mgPerLsb);
	}
	uint32_t periodMs() const {
		static const uint32_t ms[] = {0, 1000, 100, 40, 20, 10, 5, 3, 1, 1};
		const uint8_t odr = reg[0x20] >> 4;
		return odr < 10 ? ms[odr] : 0;
	}
	void tick() {
		const uint32_t p = periodMs();
		if (p && !stalled && (uint32_t)(now - lastSample) >= p) { lastSample = now; ready = true; }
	}
	void write(uint8_t r, uint8_t v) override {
		if (ignoreWrites) { writes.push_back({r, v}); return; }
		MockChip::write(r, v);
		if (r == 0x20) lastSample = now;
	}
	uint8_t read(uint8_t r) override {
		tick();
		if (r == 0x27) return ready ? 0x08 : 0x00;
		if (r >= 0x28 && r <= 0x2D) {
			const uint16_t v = (uint16_t)(raw[(r - 0x28) / 2] << 4);	// left justified
			return (r & 1) ? (uint8_t)(v >> 8) : (uint8_t)(v & 0xFF);
		}
		return reg[r];
	}
	void readRun(uint8_t r, uint8_t* p, uint16_t n) override {
		const bool inc = (r & 0x80) != 0;
		const uint8_t a = r & 0x7F;
		for (uint16_t i = 0; i < n; ++i) p[i] = read(inc ? a + i : a);
		if (a == 0x28) { ready = false; ++dataReads; }
	}
};

// ---- simulated magnetometer ----
// Identification registers "H43"; x, z, y high byte first from 0x03.
struct MockMag : MockChip {
	int16_t raw[3] = {0, 0, 0};				// x, y, z counts
	MockMag() { reg[0x0A] = 'H'; reg[0x0B] = '4'; reg[0x0C] = '3'; reg[0x02] = 0x03; }	// powers up asleep
	void setGauss(float x, float y, float z, float lsbXY, float lsbZ) {
		raw[0] = (int16_t)lroundf(x * lsbXY); raw[1] = (int16_t)lroundf(y * lsbXY); raw[2] = (int16_t)lroundf(z * lsbZ);
	}
	uint8_t read(uint8_t r) override {
		static const int axis[6] = {0, 0, 2, 2, 1, 1};	// x, z, y
		if (r >= 0x03 && r <= 0x08) {
			if (reg[0x02] & 0x03) return 0;				// not converting
			const uint16_t v = (uint16_t)raw[axis[r - 0x03]];
			return ((r - 0x03) & 1) ? (uint8_t)(v & 0xFF) : (uint8_t)(v >> 8);
		}
		return reg[r];
	}
	void readRun(uint8_t r, uint8_t* p, uint16_t n) override {
		MockChip::readRun(r, p, n);
		if (r == 0x03) ++dataReads;
	}
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
		if (isRead && !failed) chip.readRun(rr, dest, n);
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
// Both halves can sit on the one simulated bus, as on the real chip.
static int g_i2cBus;	// stands in for &hi2c1
class LoopI2C : public I2CTransport {
public:
	LoopI2C(MockChip& c, uint8_t addr) : I2CTransport(&g_i2cBus, addr, nullptr), chip(c) {}
protected:
	bool halMemWrite(uint8_t reg, uint8_t* p, uint16_t size) override {
		if (chip.present) for (uint16_t i = 0; i < size; ++i) chip.write(reg + i, p[i]);
		BusTransport::onTransferComplete(&g_i2cBus, !chip.present);	// what the I2C interrupt does
		return true;
	}
	bool halMemRead(uint8_t reg, uint8_t* p, uint16_t size) override {
		if (chip.present) chip.readRun(reg, p, size);
		BusTransport::onTransferComplete(&g_i2cBus, !chip.present);
		return true;
	}
	bool halMasterTransmit(uint8_t*, uint16_t) override { return false; }
	bool halMasterReceive(uint8_t*, uint16_t) override { return false; }
	bool halIsDeviceReady(uint32_t, uint32_t) override { return chip.present; }
private:
	MockChip& chip;
};
class AccelI2C : public LoopI2C { public: explicit AccelI2C(MockChip& c) : LoopI2C(c, LSM303DLHC_ACCEL_ADDR) {} };
class MagI2C : public LoopI2C { public: explicit MagI2C(MockChip& c) : LoopI2C(c, LSM303DLHC_MAG_ADDR) {} };

// ---- harness ----
static int g_failures = 0;
static void check(bool ok, const char* what) {
	std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok) ++g_failures;
}

struct Vec { float x = NAN, y = NAN, z = NAN; };
template <typename T> static void get(lsm303dlhc_accel<T>& s, Vec& v) { s.getAccel(&v.x, &v.y, &v.z); }
template <typename T> static void get(lsm303dlhc_mag<T>& s, Vec& v) { s.getMag(&v.x, &v.y, &v.z); }

// Runs main() once per ms, like a bare metal loop. Returns readings seen.
template <typename TSensor>
int run(TSensor& s, MockChip& chip, uint32_t start, uint32_t ms, Vec* last = nullptr) {
	int readings = 0;
	for (uint32_t i = 0; i < ms; ++i) {
		chip.now = start + i;
		s.main(chip.now);
		if (s.newData()) { Vec v; get(s, v); if (last) *last = v; ++readings; }
	}
	return readings;
}

static lsm303dlhc_accel_param_t accelParam() {
	lsm303dlhc_accel_param_t p = {};
	p.odr = lsm303dlhc_accel_100hz;
	p.scale = lsm303dlhc_accel_2g;
	p.period_ms = 100;
	return p;
}

static lsm303dlhc_mag_param_t magParam() {
	lsm303dlhc_mag_param_t p = {};
	p.rate = lsm303dlhc_mag_75hz;
	p.gain = lsm303dlhc_mag_1_3g;
	p.period_ms = 100;
	return p;
}

static bool near(float a, float b, float tol) { return std::fabs(a - b) <= tol; }

template <typename TTransport>
void accelRun(const char* name) {
	std::printf("%s\n", name);
	MockAccel chip; chip.setG(0.5f, -1.0f, 0.25f, 1.0f);
	lsm303dlhc_accel<TTransport> sensor(accelParam(), chip);
	Vec v; get(sensor, v);
	check(std::isnan(v.x) && std::isnan(v.y) && std::isnan(v.z), "NAN before the first reading");
	const int n = run(sensor, chip, 0, 1050, &v);
	std::printf("        x = %.3f  y = %.3f  z = %.3f m/s^2   (%d readings in 1.05 s)\n", v.x, v.y, v.z, n);
	const float lsb = 0.001f * G;
	check(near(v.x, 0.5f * G, lsb) && near(v.y, -G, lsb) && near(v.z, 0.25f * G, lsb), "0.5 g, -1 g and 0.25 g, to 1 mg");
	// The period runs from the end of one reading, and a reading can
	// wait up to one sample (10 ms at 100 Hz) for fresh data.
	check(n >= 9 && n <= 10, "a reading about every 100 ms");
	check(chip.reg[0x20] == 0x57, "CTRL_REG1_A = 100 Hz, normal mode, x y z on (0x57)");
	check(chip.reg[0x23] == 0x88, "CTRL_REG4_A = block data update, +-2 g, high resolution (0x88)");
	check(chip.dataReads == n, "each reading is one auto-increment read of all six bytes");
}

template <typename TTransport>
void magRun(const char* name) {
	std::printf("%s\n", name);
	MockMag chip; chip.setGauss(0.2f, -0.35f, 0.45f, 1100.0f, 980.0f);
	lsm303dlhc_mag<TTransport> sensor(magParam(), chip);
	Vec v; get(sensor, v);
	check(std::isnan(v.x) && std::isnan(v.y) && std::isnan(v.z), "NAN before the first reading");
	const int n = run(sensor, chip, 0, 1050, &v);
	std::printf("        x = %.2f  y = %.2f  z = %.2f uT   (%d readings in 1.05 s)\n", v.x, v.y, v.z, n);
	check(near(v.x, 20.0f, 0.1f) && near(v.y, -35.0f, 0.1f) && near(v.z, 45.0f, 0.11f), "20, -35 and 45 uT, with z's own sensitivity");
	check(n == 10, "a reading every 100 ms");
	check(chip.reg[0x00] == 0x18 && chip.reg[0x01] == 0x20 && chip.reg[0x02] == 0x00,
		"CRA_REG_M = 75 Hz (0x18), CRB_REG_M = +-1.3 gauss (0x20), MR_REG_M = continuous (0x00)");
	check(chip.dataReads == n, "each reading is one six byte read");
}

int main() {
	accelRun<MockBus>("lsm303dlhc_accel<MockBus>: bare ISensorTransport, asynchronous");
	accelRun<AccelI2C>("lsm303dlhc_accel<AccelI2C>: itransport's I2CTransport");
	magRun<MockBus>("lsm303dlhc_mag<MockBus>: bare ISensorTransport, asynchronous");
	magRun<MagI2C>("lsm303dlhc_mag<MagI2C>: itransport's I2CTransport");

	std::printf("accelerometer at +-16 g\n");
	{
		MockAccel chip; chip.setG(10.0f, -15.0f, 0.0f, 12.0f);
		lsm303dlhc_accel_param_t p = accelParam(); p.scale = lsm303dlhc_accel_16g;
		lsm303dlhc_accel<AccelI2C> sensor(p, chip);
		Vec v; run(sensor, chip, 0, 300, &v);
		const float lsb = 0.012f * G;
		check(chip.reg[0x23] == 0xB8, "CTRL_REG4_A = 0xB8");
		check(near(v.x, 10.0f * G, lsb) && near(v.y, -15.0f * G, lsb) && near(v.z, 0.0f, lsb), "10 g and -15 g, at 12 mg a count");
	}

	std::printf("magnetometer at +-8.1 gauss, and an axis that overflows\n");
	{
		MockMag chip; chip.setGauss(5.0f, -7.5f, 0.0f, 230.0f, 205.0f); chip.raw[2] = -4096;
		lsm303dlhc_mag_param_t p = magParam(); p.gain = lsm303dlhc_mag_8_1g;
		lsm303dlhc_mag<MagI2C> sensor(p, chip);
		Vec v; run(sensor, chip, 0, 300, &v);
		check(chip.reg[0x01] == 0xE0, "CRB_REG_M = 0xE0");
		check(near(v.x, 500.0f, 0.5f) && near(v.y, -750.0f, 0.5f), "500 and -750 uT");
		check(std::isnan(v.z), "the overflowed axis (-4096) is NAN");
	}

	std::printf("both halves on one I2C bus\n");
	{
		MockAccel accelChip; accelChip.setG(0.0f, 0.0f, 1.0f, 1.0f);
		MockMag magChip; magChip.setGauss(0.1f, 0.2f, 0.3f, 1100.0f, 980.0f);
		lsm303dlhc_accel<AccelI2C> accel(accelParam(), accelChip);
		lsm303dlhc_mag<MagI2C> mag(magParam(), magChip);
		int na = 0, nm = 0; Vec va, vm;
		for (uint32_t t = 0; t < 2050; ++t) {
			accelChip.now = magChip.now = t;
			accel.main(t); mag.main(t);
			if (accel.newData()) { get(accel, va); ++na; }
			if (mag.newData()) { get(mag, vm); ++nm; }
		}
		std::printf("        %d accelerometer and %d magnetometer readings in 2.05 s\n", na, nm);
		check(na >= 18 && na <= 20 && nm >= 18 && nm <= 20, "both read about every 100 ms from the same loop");
		check(near(va.z, G, 0.01f) && near(vm.y, 20.0f, 0.1f), "and each gets its own chip's values");
	}

	std::printf("startMeasurement(): a reading ahead of the period\n");
	{
		MockMag chip;
		lsm303dlhc_mag_param_t p = magParam(); p.period_ms = 1000;
		lsm303dlhc_mag<MagI2C> sensor(p, chip);
		run(sensor, chip, 0, 1100);
		sensor.startMeasurement();
		check(run(sensor, chip, 1100, 120) == 1, "taken up within 120 ms instead of waiting out the second");
	}

	std::printf("devices absent, then plugged in\n");
	{
		MockAccel ac; ac.present = false;
		MockMag mc; mc.present = false;
		lsm303dlhc_accel<AccelI2C> accel(accelParam(), ac);
		lsm303dlhc_mag<MagI2C> mag(magParam(), mc);
		check(run(accel, ac, 0, 1000) == 0 && run(mag, mc, 0, 1000) == 0, "no readings while absent");
		ac.present = mc.present = true;
		check(run(accel, ac, 1000, 1000) >= 1 && run(mag, mc, 1000, 1000) >= 1, "both recover by themselves");
	}

	std::printf("wrong chips at the addresses\n");
	{
		MockAccel ac; ac.ignoreWrites = true;
		lsm303dlhc_accel<MockBus> accel(accelParam(), ac);
		check(run(accel, ac, 0, 2000) == 0, "accelerometer: CTRL_REG1_A does not read back, so no readings");
		check(ac.count(0x23, 0x88) == 0, "and it is never configured further");
		MockMag mc; mc.reg[0x0B] = '5';
		lsm303dlhc_mag<MockBus> mag(magParam(), mc);
		check(run(mag, mc, 0, 2000) == 0 && mc.writes.empty(), "magnetometer: wrong identification, no readings and no writes");
	}

	std::printf("accelerometer that stops sampling\n");
	{
		MockAccel chip;
		lsm303dlhc_accel<MockBus> sensor(accelParam(), chip);
		run(sensor, chip, 0, 500);
		chip.stalled = true;
		check(run(sensor, chip, 500, 3000) == 0, "no reading reported");
		Vec v; get(sensor, v);
		check(std::isnan(v.x) && chip.count(0x20, 0x57) >= 2, "times out, marks the reading NAN and starts again");
		chip.stalled = false;
		check(run(sensor, chip, 3500, 1000) >= 1, "reads again once samples come");
	}

	std::printf("transfer that never completes\n");
	{
		MockAccel chip;
		lsm303dlhc_accel<MockBus> sensor(accelParam(), chip);
		run(sensor, chip, 0, 500);
		chip.stuck = true;
		run(sensor, chip, 500, 1000);
		check(sensor.state() == lsm303dlhc_accel_error_state, "gives up after the bus timeout");
		chip.stuck = false;
		check(run(sensor, chip, 1500, 1000) >= 1, "recovers when the bus comes back");
	}

	std::printf("transport that cannot start a transfer\n");
	{
		MockMag chip;
		lsm303dlhc_mag<MockBus> sensor(magParam(), chip);
		run(sensor, chip, 0, 500);
		const size_t before = chip.writes.size();
		chip.refuse = true;
		check(run(sensor, chip, 500, 1500) == 0 && chip.writes.size() == before, "nothing reaches the bus");
		chip.refuse = false;
		check(run(sensor, chip, 2000, 1000) >= 1, "recovers when the bus is free");
	}

	std::printf("tick counter rollover\n");
	{
		MockAccel chip;
		lsm303dlhc_accel<AccelI2C> sensor(accelParam(), chip);
		const uint32_t start = 0xFFFFFFFFu - 500;	// wraps half a second into the run
		check(run(sensor, chip, start, 1050) == 10, "readings keep their rhythm across the wrap");
	}

	std::printf("xlsm303dlhc: sleep times under an OS\n");
	{
		MockMag chip;
		lsm303dlhc_mag_param_t p = magParam(); p.period_ms = 1000;
		xlsm303dlhc_mag<MockBus> sensor(p, chip);
		// Like a thread: main(), then advance the clock by however long
		// main() slept, plus the one tick the loop gives up every cycle.
		uint32_t now = 0; int calls = 0, readings = 0; uint32_t longest = 0;
		g_delays.clear();
		while (now < 10000) {
			chip.now = now; g_slept = 0;
			sensor.main(now); ++calls;
			if (sensor.newData()) { Vec v; get(sensor, v); ++readings; }
			now += g_slept + 1;
		}
		for (uint32_t d : g_delays) if (d > longest) longest = d;
		std::printf("        %d readings in 10 s from %d calls to main(), longest sleep %u ms\n", readings, calls, (unsigned)longest);
		check(readings >= 9, "still reads on schedule");
		check(longest == 100, "never sleeps longer than 100 ms, so a startMeasurement() request is seen promptly");
		check(calls < 300, "wakes only when there is something to do");

		MockAccel ac;
		xlsm303dlhc_accel<MockBus> accel(accelParam(), ac);
		now = 0; readings = 0;
		while (now < 2000) { ac.now = now; g_slept = 0; accel.main(now); if (accel.newData()) { Vec v; get(accel, v); ++readings; } now += g_slept + 1; }
		check(readings >= 18, "xlsm303dlhc_accel reads on schedule too");
	}

	std::printf("\n%s\n", g_failures ? "FAILED" : "all passed");
	return g_failures ? 1 : 0;
}
