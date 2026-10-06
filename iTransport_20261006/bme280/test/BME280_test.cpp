/*
 * BME280_test.cpp
 *
 * Host test for bme280<TTransport>. No hardware, HAL or RTOS needed:
 *
 *   g++ -std=c++17 -Wall -Wextra -I../inc -Istub \
 *       -I<safeTransport>/itransport/inc -I<safeTransport>/isensor/inc \
 *       BME280_test.cpp <safeTransport>/itransport/src/BusTransport.cpp \
 *       <safeTransport>/itransport/src/I2CTransport.cpp \
 *       <safeTransport>/itransport/src/SPITransport.cpp -o BME280_test
 *
 * Temperature and pressure use the worked example in the Bosch
 * datasheet (25.08 C, 100653.27 Pa). The datasheet has no worked
 * example for humidity, so the driver's floating point result is
 * checked against the datasheet's separate integer formula, coded
 * independently below.
 */

#include <cstdio>
#include <cmath>
#include <vector>
#include <utility>
#include "I2CTransport.h"
#include "SPITransport.h"
#include "xBME280.h"	// pulls in BME280.h. stub/cmsis_os2.h stands in for the RTOS

using namespace BME280;

uint32_t g_slept = 0;						// set by the stub osDelay()
std::vector<uint32_t> g_delays;

struct HumCal { int H1, H2, H3, H4, H5, H6; };

// bme280_compensate_H_int32() from the datasheet. Returns %RH.
static double referenceHumidity(int32_t adc_H, int32_t t_fine, const HumCal& c) {
	int32_t v = t_fine - 76800;
	v = ((((adc_H << 14) - (c.H4 * 1048576) - (c.H5 * v)) + 16384) >> 15) *
		(((((((v * c.H6) >> 10) * (((v * c.H3) >> 11) + 32768)) >> 10) + 2097152) * c.H2 + 8192) >> 14);
	v = v - (((((v >> 15) * (v >> 15)) >> 7) * c.H1) >> 4);
	if (v < 0) v = 0;
	if (v > 419430400) v = 419430400;
	return (v >> 12) / 1024.0;
}

// ---- simulated BME280 ----
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
		reg[0xD0] = 0x60;
		const int16_t cal[12] = {27504, 26435, -1000, (int16_t)36477, -10685, 3024,
								  2855, 140, -7, 15500, -14600, 6000};
		for (int i = 0; i < 12; ++i) {
			reg[0x88 + 2*i]     = (uint8_t)(cal[i] & 0xFF);
			reg[0x88 + 2*i + 1] = (uint8_t)((cal[i] >> 8) & 0xFF);
		}
		setHumCal({75, 355, 0, 333, 50, 30});
		const uint32_t adcP = 415148, adcT = 519888;
		reg[0xF7] = adcP >> 12; reg[0xF8] = (adcP >> 4) & 0xFF; reg[0xF9] = (adcP & 0xF) << 4;
		reg[0xFA] = adcT >> 12; reg[0xFB] = (adcT >> 4) & 0xFF; reg[0xFC] = (adcT & 0xF) << 4;
		setAdcH(30281);
	}
	void setHumCal(const HumCal& c) {		// laid out as in the chip's registers
		reg[0xA1] = (uint8_t)c.H1;
		reg[0xE1] = (uint8_t)(c.H2 & 0xFF); reg[0xE2] = (uint8_t)((c.H2 >> 8) & 0xFF);
		reg[0xE3] = (uint8_t)c.H3;
		reg[0xE4] = (uint8_t)((c.H4 >> 4) & 0xFF);
		reg[0xE5] = (uint8_t)((c.H4 & 0x0F) | ((c.H5 & 0x0F) << 4));
		reg[0xE6] = (uint8_t)((c.H5 >> 4) & 0xFF);
		reg[0xE7] = (uint8_t)c.H6;
	}
	void setAdcH(uint16_t adcH) { reg[0xFD] = adcH >> 8; reg[0xFE] = adcH & 0xFF; }
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
	int indexOf(uint8_t r, uint8_t v) const {
		for (size_t i = 0; i < writes.size(); ++i) if (writes[i].first == r && writes[i].second == v) return (int)i;
		return -1;
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
	explicit LoopI2C(MockChip& c) : I2CTransport(&g_i2cBus, bme280_i2c_addr_2, nullptr), chip(c) {}
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
	bool halTransmit(uint8_t* tx, uint16_t len) override {			// BME280 SPI write: bit 7 cleared
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
int run(TSensor& s, MockChip& chip, uint32_t start, uint32_t ms, double* t = nullptr, double* p = nullptr, double* h = nullptr) {
	int readings = 0;
	for (uint32_t i = 0; i < ms; ++i) {
		chip.now = start + i;
		s.main(chip.now);
		if (s.newData()) {
			double tt, pp, hh;
			s.GetTemperature(&tt); s.GetPressure(&pp); s.GetHumidity(&hh);
			if (t) *t = tt;
			if (p) *p = pp;
			if (h) *h = hh;
			++readings;
		}
	}
	return readings;
}

static const bme280_param_t kParam = {bme280_osrs_p_1x, bme280_osrs_t_1x, bme280_osrs_h_1x,
									  bme280_forced_mode, bme280_sb_125, bme280_filter_off};
static const int32_t kTFine = 128422;		// t_fine for the datasheet's temperature example

template <typename TTransport>
void datasheetVector(const char* name) {
	std::printf("%s\n", name);
	MockChip chip;
	bme280_param_t param = kParam; param._filter_t = bme280_filter_x4;
	bme280<TTransport> sensor(param, chip);
	double t = NAN, p = NAN, h = NAN;
	check(!sensor.GetTemperature(&t) && !sensor.GetHumidity(&h) && std::isnan(t) && std::isnan(h), "no reading claimed before the first measurement");
	int n = run(sensor, chip, 0, 3000, &t, &p, &h);
	const double href = referenceHumidity(30281, kTFine, {75, 355, 0, 333, 50, 30});
	std::printf("        T = %.2f C   P = %.2f Pa   H = %.2f %%RH (reference %.2f)   (%d readings in 3 s)\n", t, p, h, href, n);
	check(std::fabs(t - 25.08) < 0.005, "temperature matches the datasheet example, 25.08 C");
	// The datasheet prints 100653.27. Its double formula evaluated at full
	// precision gives 100653.258 and its 64 bit integer formula 100653.254.
	check(std::fabs(p - 100653.27) < 0.05, "pressure matches the datasheet example, 100653.27 Pa");
	check(std::fabs(h - href) < 0.01, "humidity agrees with the datasheet's integer formula");
	check(n >= 12 && n <= 15, "a reading roughly every 210 ms");
	check(chip.resets == 1 && chip.indexOf(0xE0, 0xB6) >= 0, "soft reset issued once");
	check(chip.indexOf(0xF2, 0x01) >= 0, "ctrl_hum = osrs_h x1 (0x01)");
	check(chip.indexOf(0xF5, 0x48) >= 0, "config = t_sb 125 ms, filter x4, 4 wire (0x48)");
	check(chip.indexOf(0xF4, 0x26) >= 0, "ctrl_meas = osrs_t x1, osrs_p x1, forced (0x26)");
	check(chip.indexOf(0xF2, 0x01) < chip.indexOf(0xF4, 0x26), "ctrl_hum written before ctrl_meas, as the chip requires");
}

int main() {
	datasheetVector<MockBus>("bme280<MockBus>: bare ISensorTransport, asynchronous");
	datasheetVector<LoopI2C>("bme280<LoopI2C>: itransport's I2CTransport");
	datasheetVector<LoopSPI>("bme280<LoopSPI>: itransport's SPITransport");

	std::printf("humidity across its range\n");
	{
		const struct { const char* what; HumCal cal; uint16_t adcH; } cases[] = {
			{"negative dig_H4 and dig_H5 are read as signed", {75, 355, 0, -20, -30, 30}, 8700},
			{"non-zero dig_H3",                              {75, 370, 12, 310, 45, 30}, 26000},
			{"dry end",                                      {75, 355, 0, 333, 50, 30}, 22500},
			{"humid end",                                    {75, 355, 0, 333, 50, 30}, 38500},
			{"below 0 is held at 0",                         {75, 355, 0, 333, 50, 30}, 5000},
			{"above 100 is held at 100",                     {75, 355, 0, 333, 50, 30}, 60000},
		};
		for (const auto& c : cases) {
			MockChip chip; chip.setHumCal(c.cal); chip.setAdcH(c.adcH);
			bme280<LoopI2C> sensor(kParam, chip);
			double h = NAN; run(sensor, chip, 0, 600, nullptr, nullptr, &h);
			const double href = referenceHumidity(c.adcH, kTFine, c.cal);
			std::printf("        %-48s %7.3f %%RH (reference %7.3f)\n", c.what, h, href);
			check(std::fabs(h - href) < 0.02, c.what);
		}
	}

	std::printf("newData() is true once per measurement\n");
	{
		MockChip chip;
		bme280<LoopI2C> sensor(kParam, chip);
		int seen = 0;
		for (uint32_t i = 0; i < 400 && seen == 0; ++i) { chip.now = i; sensor.main(i); if (sensor.newData()) ++seen; }
		check(seen == 1 && !sensor.newData(), "asking again without another measurement says no");
	}

	std::printf("device absent, then plugged in\n");
	{
		MockChip chip; chip.present = false;
		bme280<LoopI2C> sensor(kParam, chip);
		check(run(sensor, chip, 0, 2000) == 0, "no readings while absent");
		chip.present = true;
		check(run(sensor, chip, 2000, 2000) >= 1, "recovers by itself once the device answers");
	}

	std::printf("device absent on SPI, where there is no acknowledge\n");
	{
		MockChip chip; chip.present = false;
		bme280<LoopSPI> sensor(kParam, chip);
		check(run(sensor, chip, 0, 2000) == 0, "ID check rejects a bus that reads 0xFF");
	}

	std::printf("wrong chip at the address\n");
	{
		MockChip chip; chip.reg[0xD0] = 0x58;	// a BMP280
		bme280<MockBus> sensor(kParam, chip);
		check(run(sensor, chip, 0, 3000) == 0, "no readings from a chip with the wrong ID");
		check(chip.resets > 5 && chip.resets < 20, "keeps retrying from init, with a pause each time");
	}

	std::printf("transfer that never completes\n");
	{
		MockChip chip;
		bme280<MockBus> sensor(kParam, chip);
		run(sensor, chip, 0, 500);
		chip.stuck = true;
		run(sensor, chip, 500, 2000);
		check(sensor.state() == bme280_error, "gives up after the bus timeout");
		chip.stuck = false;
		check(run(sensor, chip, 2500, 2000) >= 1, "recovers when the bus comes back");
	}

	std::printf("transport that cannot start a transfer\n");
	{
		MockChip chip;
		bme280<MockBus> sensor(kParam, chip);
		run(sensor, chip, 0, 500);
		size_t before = chip.writes.size();
		chip.refuse = true;
		check(run(sensor, chip, 500, 2000) == 0 && chip.writes.size() == before, "nothing reaches the bus");
		chip.refuse = false;
		check(run(sensor, chip, 2500, 2000) >= 1, "recovers when the bus is free");
	}

	std::printf("measuring bit stuck high\n");
	{
		MockChip chip;
		bme280<MockBus> sensor(kParam, chip);
		run(sensor, chip, 0, 500);
		const int before = chip.resets;
		chip.measuringStuck = true;
		check(run(sensor, chip, 500, 4000) == 0, "no reading reported");
		check(chip.resets > before, "times out and re-initialises instead of waiting for ever");
	}

	std::printf("tick counter rollover\n");
	{
		MockChip chip;
		bme280<LoopI2C> sensor(kParam, chip);
		const uint32_t start = 0xFFFFFFFFu - 2500;	// wraps 2.5 s into the run
		const int before = run(sensor, chip, start, 2400);
		const int across = run(sensor, chip, start + 2400, 400);		// the 400 ms either side of the wrap
		const int after  = run(sensor, chip, start + 2800, 2400);
		check(before >= 10 && across >= 1 && after >= 10, "readings keep coming before, across and after the wrap");
	}

	std::printf("xbme280: sleep times under an OS\n");
	{
		MockChip chip;
		xbme280<MockBus> sensor(kParam, chip);
		// Like a thread: main(), then advance the clock by however long
		// main() slept, plus the one tick the loop gives up every cycle.
		uint32_t now = 0; int calls = 0, readings = 0;
		g_delays.clear();
		while (now < 10000) {
			chip.now = now; g_slept = 0;
			sensor.main(now); ++calls;
			if (sensor.newData()) ++readings;
			now += g_slept + 1;
		}
		int longSleeps = 0;
		for (uint32_t d : g_delays) if (d >= 90 && d <= 100) ++longSleeps;
		std::printf("        %d readings in 10 s from %d calls to main()\n", readings, calls);
		check(longSleeps >= 2 * readings - 2, "sleeps the whole wait between measurements and the whole conversion");
		check(readings >= 40, "still measures on schedule");
		check(calls < readings * 20, "wakes only when there is something to do");
	}

	std::printf("\n%s\n", g_failures ? "FAILED" : "all passed");
	return g_failures ? 1 : 0;
}
