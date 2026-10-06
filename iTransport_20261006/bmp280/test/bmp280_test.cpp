/*
 * bmp280_test.cpp
 *
 * Host test for bmp280<TTransport>. No hardware, HAL or RTOS needed:
 *
 *   g++ -std=c++17 -Wall -Wextra -I../inc -Istub \
 *       -I<safeTransport>/itransport/inc -I<safeTransport>/isensor/inc \
 *       bmp280_test.cpp <safeTransport>/itransport/src/BusTransport.cpp \
 *       <safeTransport>/itransport/src/I2CTransport.cpp \
 *       <safeTransport>/itransport/src/SPITransport.cpp -o bmp280_test
 *
 * The simulated chip holds the calibration and raw values of the
 * worked example in the BMP280 datasheet, which gives 25.08 C and
 * 100653.27 Pa.
 */

#include <cstdio>
#include <cmath>
#include <vector>
#include <utility>
#include "I2CTransport.h"
#include "SPITransport.h"
#include "xbmp280.h"	// pulls in bmp280.h. stub/cmsis_os2.h stands in for the RTOS

using namespace BMP280;

uint32_t g_slept = 0;						// set by the stub osDelay()
std::vector<uint32_t> g_delays;

// ---- simulated BMP280 ----
struct MockChip {
	uint8_t reg[256] = {0};
	uint32_t now = 0;						// the test's clock
	bool present = true;
	bool stuck = false;						// transfers never complete
	bool refuse = false;					// transport cannot start a transfer
	bool measuringStuck = false;
	uint32_t imUpdateUntil = 0;
	uint32_t convEnd = 0;
	int resets = 0;
	std::vector<std::pair<uint8_t,uint8_t>> writes;

	MockChip() {
		reg[0xD0] = 0x58;
		const int16_t cal[12] = {27504, 26435, -1000, (int16_t)36477, -10685, 3024,
								  2855, 140, -7, 15500, -14600, 6000};
		for (int i = 0; i < 12; ++i) {
			reg[0x88 + 2*i]     = (uint8_t)(cal[i] & 0xFF);
			reg[0x88 + 2*i + 1] = (uint8_t)((cal[i] >> 8) & 0xFF);
		}
		const uint32_t adcP = 415148, adcT = 519888;
		reg[0xF7] = adcP >> 12; reg[0xF8] = (adcP >> 4) & 0xFF; reg[0xF9] = (adcP & 0xF) << 4;
		reg[0xFA] = adcT >> 12; reg[0xFB] = (adcT >> 4) & 0xFF; reg[0xFC] = (adcT & 0xF) << 4;
	}
	void write(uint8_t r, uint8_t v) {
		writes.push_back({r, v});
		if (r == 0xE0 && v == 0xB6) { ++resets; imUpdateUntil = now + 3; return; }
		reg[r] = v;
		if (r == 0xF4 && (v & 0x03)) convEnd = now + 8;
	}
	uint8_t read(uint8_t r) const {
		if (r != 0xF3) return reg[r];
		uint8_t s = 0;
		if ((int32_t)(now - imUpdateUntil) < 0) s |= 0x01;
		if (measuringStuck || (int32_t)(now - convEnd) < 0) s |= 0x08;
		return s;
	}
	bool wrote(uint8_t r, uint8_t v) const {
		for (auto& w : writes) if (w.first == r && w.second == v) return true;
		return false;
	}
};

// ---- transport 1: a bare ISensorTransport whose transfers take time ----
class MockBus : public ISensorTransport {
public:
	explicit MockBus(MockChip& c) : chip(c) {}
	bool writeReg(uint8_t r, uint8_t v) override {
		if (busy || chip.refuse) return false;
		failed = !chip.present;
		if (chip.present) chip.write(r, v);
		busy = true; polls = 1; isRead = false;
		return true;
	}
	bool readRegs(uint8_t r, uint8_t* buf, uint8_t len) override {
		if (busy || chip.refuse) return false;
		failed = !chip.present;
		busy = true; polls = 1; isRead = true; rr = r; dest = buf; n = len;
		return true;
	}
	// bmp280 is register-addressed and uses none of these three, but
	// the interface requires them.
	bool writeRegs(uint8_t r, const uint8_t* buf, uint8_t len) override {
		if (busy || chip.refuse) return false;
		failed = !chip.present;
		if (chip.present) for (uint8_t i = 0; i < len; ++i) chip.write(r + i, buf[i]);
		busy = true; polls = 1; isRead = false;
		return true;
	}
	bool writeBytes(const uint8_t*, uint8_t) override { return false; }
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
	explicit LoopI2C(MockChip& c) : I2CTransport(&g_i2cBus, bmp280_i2c_addr_2, nullptr), chip(c) {}
protected:
	bool halMemWrite(uint8_t reg, uint8_t* p, uint16_t size) override {
		if (chip.present) for (uint16_t i = 0; i < size; ++i) chip.write(reg + i, p[i]);
		I2CTransport::onTransferComplete(&g_i2cBus, !chip.present);
		return true;
	}
	bool halMemRead(uint8_t reg, uint8_t* p, uint16_t size) override {
		if (chip.present) for (uint16_t i = 0; i < size; ++i) p[i] = chip.read(reg + i);
		I2CTransport::onTransferComplete(&g_i2cBus, !chip.present);
		return true;
	}
	bool halMasterTransmit(uint8_t*, uint16_t) override { return false; }	// not used by a register-addressed chip
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
	bool halTransmit(uint8_t* tx, uint16_t len) override {			// BMP280 SPI write: bit 7 cleared
		if (chip.present) for (uint16_t i = 1; i < len; ++i) chip.write((tx[0] | 0x80) + (i - 1), tx[i]);
		BusTransport::onTransferComplete(&g_spiBus, false);			// what the SPI interrupt does
		return true;
	}
	bool halTransmitReceive(uint8_t* tx, uint8_t* rx, uint16_t len) override {
		rx[0] = 0xFF;
		for (uint16_t i = 1; i < len; ++i) rx[i] = chip.present ? chip.read(tx[0] + (i - 1)) : 0xFF;
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
int run(TSensor& s, MockChip& chip, uint32_t start, uint32_t ms, double* t = nullptr, double* p = nullptr) {
	int readings = 0;
	for (uint32_t i = 0; i < ms; ++i) {
		chip.now = start + i;
		s.main(chip.now);
		if (s.newData()) {
			double tt, pp;
			s.GetTemperature(&tt); s.GetPressure(&pp);
			if (t) *t = tt;
			if (p) *p = pp;
			++readings;
		}
	}
	return readings;
}

template <typename TTransport>
void datasheetVector(const char* name) {
	std::printf("%s\n", name);
	MockChip chip;
	bmp280_param_t param = {bmp280_osrs_p_1x, bmp280_osrs_t_1x, bmp280_forced_mode, bmp280_sb_125, bmp280_filter_x4};
	bmp280<TTransport> sensor(param, chip);
	double t = NAN, p = NAN;
	check(!sensor.GetTemperature(&t) && std::isnan(t), "no reading claimed before the first measurement");
	int n = run(sensor, chip, 0, 3000, &t, &p);
	std::printf("        T = %.2f C   P = %.2f Pa   (%d readings in 3 s)\n", t, p, n);
	check(std::fabs(t - 25.08) < 0.005, "temperature matches the datasheet example, 25.08 C");
	// The datasheet prints 100653.27. Its double formula evaluated at full
	// precision gives 100653.258 and its 64 bit integer formula 100653.254.
	check(std::fabs(p - 100653.27) < 0.05, "pressure matches the datasheet example, 100653.27 Pa");
	check(n == 2, "one reading per period");
	check(chip.resets == 1 && chip.wrote(0xE0, 0xB6), "soft reset issued once");
	check(chip.wrote(0xF5, 0x48), "config = t_sb 125 ms, filter x4, 4 wire (0x48)");
	check(chip.wrote(0xF4, 0x26), "ctrl_meas = osrs_t x1, osrs_p x1, forced (0x26)");
}

int main() {
	bmp280_param_t param = {bmp280_osrs_p_1x, bmp280_osrs_t_1x, bmp280_forced_mode, bmp280_sb_125, bmp280_filter_off};

	datasheetVector<MockBus>("bmp280<MockBus>: bare ISensorTransport, asynchronous");
	datasheetVector<LoopI2C>("bmp280<LoopI2C>: itransport's I2CTransport");
	datasheetVector<LoopSPI>("bmp280<LoopSPI>: itransport's SPITransport");

	std::printf("device absent, then plugged in\n");
	{
		MockChip chip; chip.present = false;
		bmp280<LoopI2C> sensor(param, chip);
		check(run(sensor, chip, 0, 2000) == 0, "no readings while absent");
		chip.present = true;
		check(run(sensor, chip, 2000, 2000) >= 1, "recovers by itself once the device answers");
	}

	std::printf("device absent on SPI, where checkDevice() cannot tell\n");
	{
		MockChip chip; chip.present = false;
		bmp280<LoopSPI> sensor(param, chip);
		check(run(sensor, chip, 0, 2000) == 0, "ID check rejects a bus that reads 0xFF");
	}

	std::printf("wrong chip at the address\n");
	{
		MockChip chip; chip.reg[0xD0] = 0x60;	// a BME280
		bmp280<MockBus> sensor(param, chip);
		check(run(sensor, chip, 0, 3000) == 0, "no readings from a chip with the wrong ID");
		check(chip.resets > 5, "keeps retrying from init");
	}

	std::printf("transfer that never completes\n");
	{
		MockChip chip;
		bmp280<MockBus> sensor(param, chip);
		run(sensor, chip, 0, 1500);
		chip.stuck = true;
		run(sensor, chip, 1500, 2000);
		check(sensor.state() == bmp280_error || sensor.state() == bmp280_init ||
			  sensor.state() == bmp280_reset, "gives up after the bus timeout");
		chip.stuck = false;
		check(run(sensor, chip, 3500, 3000) >= 1, "recovers when the bus comes back");
	}

	std::printf("transport that cannot start a transfer\n");
	{
		MockChip chip;
		bmp280<MockBus> sensor(param, chip);
		run(sensor, chip, 0, 1500);
		size_t before = chip.writes.size();
		chip.refuse = true;
		run(sensor, chip, 1500, 2000);
		check(chip.writes.size() == before, "nothing reaches the bus");
		chip.refuse = false;
		check(run(sensor, chip, 3500, 3000) >= 1, "recovers when the bus is free");
	}

	std::printf("measuring bit stuck high\n");
	{
		MockChip chip;
		bmp280<MockBus> sensor(param, chip);
		run(sensor, chip, 0, 1100);
		chip.measuringStuck = true;
		check(run(sensor, chip, 1100, 4000) == 0, "no reading reported");
		check(chip.resets >= 2, "times out and re-initialises");
	}

	std::printf("tick counter rollover\n");
	{
		MockChip chip;
		bmp280<LoopI2C> sensor(param, chip);
		uint32_t start = 0xFFFFFFFFu - 2500;	// wraps 2.5 s into the run
		check(run(sensor, chip, start, 6000) == 4, "readings keep their 1.2 s rhythm across the wrap");
	}

	std::printf("xbmp280: sleep times under an OS\n");
	{
		MockChip chip;
		xbmp280<MockBus> sensor(param, chip);
		// Like a thread: main(), then advance the clock by however long
		// main() slept, plus the one tick the loop gives up every cycle.
		uint32_t now = 0; int calls = 0, readings = 0;
		g_delays.clear();
		while (now < 10000) {
			chip.now = now; g_slept = 0;
			sensor.main(now); ++calls;
			if (sensor.newData()) { double v; sensor.GetPressure(&v); ++readings; }
			now += g_slept + 1;
		}
		bool period = false, conversion = false;
		for (uint32_t d : g_delays) { if (d >= 990 && d <= 1000) period = true; if (d >= 190 && d <= 200) conversion = true; }
		std::printf("        %d readings in 10 s from %d calls to main()\n", readings, calls);
		check(period, "sleeps the whole period between measurements");
		check(conversion, "sleeps the whole conversion time");
		check(readings >= 7, "still measures on schedule");
		check(calls < 400, "wakes only when there is something to do");
	}

	std::printf("\n%s\n", g_failures ? "FAILED" : "all passed");
	return g_failures ? 1 : 0;
}
