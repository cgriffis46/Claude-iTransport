/*
 * SHT31_test.cpp
 *
 * Host test for SHT31<TTransport>. No hardware, HAL or RTOS needed:
 *
 *   g++ -std=c++17 -Wall -Wextra -I../inc -Istub \
 *       -I<safeTransport>/itransport/inc -I<safeTransport>/isensor/inc \
 *       SHT31_test.cpp <safeTransport>/itransport/src/BusTransport.cpp \
 *       <safeTransport>/itransport/src/I2CTransport.cpp -o SHT31_test
 */

#include <cstdio>
#include <cmath>
#include <vector>
#include "I2CTransport.h"
#include "xSHT31.h"	// pulls in SHT31.h. stub/cmsis_os2.h stands in for the RTOS

uint32_t g_slept = 0;						// set by the stub osDelay()
std::vector<uint32_t> g_delays;

// CRC-8, polynomial 0x31, initial value 0xFF, as the SHT31 uses in both directions.
static uint8_t crc8(const uint8_t* p, int n) {
	uint8_t crc = 0xFF;
	for (int i = 0; i < n; ++i) {
		crc ^= p[i];
		for (int k = 0; k < 8; ++k) crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
	}
	return crc;
}

// ---- simulated SHT31: takes 16 bit commands, is read back as a run of bytes ----
struct MockChip {
	uint32_t now = 0;						// the test's clock
	bool present = true;
	bool stuck = false;						// operations never complete
	bool refuse = false;					// transport cannot start an operation
	bool corrupt = false;					// one bit of a reading flipped on the way back
	float temperature = 25.0f, humidity = 50.0f;

	bool periodic = false; uint16_t periodicCmd = 0; uint32_t lastSample = 0; uint32_t samplePeriod = 100;
	bool heater = false;
	bool measuring = false; uint32_t convEnd = 0;
	enum { Nothing, Status, Single, Fetch } pendingRead = Nothing;
	int resets = 0, singles = 0, fetches = 0, breaks = 0, nacks = 0;
	int violations = 0;						// commands sent in periodic mode without a break first, and the like
	std::vector<uint16_t> commands;
	uint16_t alert[4] = {0, 0, 0, 0};		// high set, high clear, low clear, low set, as written
	int badAlertCrc = 0;

	bool write(const uint8_t* b, uint16_t n) {			// false: not acknowledged
		if (!present || n < 2) return false;
		const uint16_t cmd = (uint16_t)((b[0] << 8) | b[1]);
		commands.push_back(cmd);
		if (periodic && cmd != 0x3093 && cmd != 0xE000) ++violations;	// datasheet: break before anything else
		if (!periodic && cmd == 0xE000) ++violations;					// fetch data belongs to periodic mode
		switch (cmd) {
		case 0x30A2: ++resets; periodic = false; heater = false; measuring = false; pendingRead = Nothing; break;
		case 0x3041: break;
		case 0xF32D: pendingRead = Status; break;
		case 0x306D: heater = true; break;
		case 0x3066: heater = false; break;
		case 0x3093: ++breaks; periodic = false; break;
		case 0x2400: ++singles; measuring = true; convEnd = now + 15; pendingRead = Single; break;
		case 0xE000: ++fetches; pendingRead = Fetch; break;
		case 0x611D: case 0x6116: case 0x610B: case 0x6100:
			if (n != 5 || crc8(b + 2, 2) != b[4]) { ++badAlertCrc; break; }
			alert[cmd == 0x611D ? 0 : cmd == 0x6116 ? 1 : cmd == 0x610B ? 2 : 3] = (uint16_t)((b[2] << 8) | b[3]);
			break;
		default:
			if ((cmd >> 8) >= 0x20 && (cmd >> 8) <= 0x27) { periodic = true; periodicCmd = cmd; lastSample = now; }
			break;
		}
		return true;
	}
	bool read(uint8_t* out, uint16_t n) {
		if (!present) return false;
		if (pendingRead == Status && n == 3) {
			const uint16_t st = (uint16_t)(heater ? 0x2000 : 0x0000);
			out[0] = st >> 8; out[1] = st & 0xFF; out[2] = crc8(out, 2);
			pendingRead = Nothing;
			return true;
		}
		if (n != 6) { ++nacks; return false; }
		if (pendingRead == Single) {
			if ((int32_t)(now - convEnd) < 0) { ++nacks; return false; }	// not ready: no acknowledge
			measuring = false;
		} else if (pendingRead == Fetch) {
			if ((now - lastSample) < samplePeriod) { ++nacks; return false; }	// nothing new since the last fetch
			lastSample = now;
		} else { ++nacks; return false; }
		pendingRead = Nothing;
		const uint16_t t = (uint16_t)lroundf((temperature + 45.0f) / 175.0f * 65535.0f);
		const uint16_t h = (uint16_t)lroundf(humidity / 100.0f * 65535.0f);
		out[0] = t >> 8; out[1] = t & 0xFF; out[2] = crc8(out, 2);
		out[3] = h >> 8; out[4] = h & 0xFF; out[5] = crc8(out + 3, 2);
		if (corrupt) out[4] ^= 0x10;
		return true;
	}
	int count(uint16_t c) const { int n = 0; for (uint16_t x : commands) if (x == c) ++n; return n; }
	int indexOf(uint16_t c, int from = 0) const { for (size_t i = from; i < commands.size(); ++i) if (commands[i] == c) return (int)i; return -1; }
	// What an alert limit, as written to the chip, stands for.
	static float alertTemp(uint16_t w) { return -45.0f + 175.0f * (float)((w & 0x01FF) << 7) / 65535.0f; }
	static float alertHum(uint16_t w) { return 100.0f * (float)(w & 0xFE00) / 65535.0f; }
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
	explicit LoopI2C(MockChip& c) : I2CTransport(&g_i2cBus, sht31_i2c_addr1, nullptr), chip(c) {}
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
			float tt, hh; s.GetTemperature(&tt); s.GetHumidity(&hh);
			if (t) *t = tt;
			if (h) *h = hh;
			++readings;
		}
	}
	return readings;
}

static const SHT31_param_t kSingle = {false, false, _1mps_high_Res};
static const SHT31_param_t kPeriodic = {true, false, _10mps_high_Res};

template <typename TTransport>
void singleShot(const char* name) {
	std::printf("%s\n", name);
	MockChip chip;
	SHT31<TTransport> sensor(kSingle, chip);
	float t = 0, h = 0;
	check(!sensor.GetTemperature(&t) && std::isnan(t), "NAN before the first reading");
	int n = run(sensor, chip, 0, 3000, &t, &h);
	std::printf("        T = %.2f C   H = %.2f %%RH   (%d reading in 3 s)\n", t, h, n);
	check(std::fabs(t - 25.0f) < 0.02f && std::fabs(h - 50.0f) < 0.02f, "temperature 25 C and humidity 50 %RH");
	check(n == 1 && chip.singles == 1, "one measurement at start up, then it waits to be asked");
	check(chip.commands == std::vector<uint16_t>({0x30A2, 0x3041, 0xF32D, 0x2400}), "commands: reset, clear status, read status, measure");
	check(chip.fetches == 0 && chip.nacks == 0, "the result is read straight back, with no fetch command and no refused read");
	check(sensor.ForceMeasurement(), "ForceMeasurement() says the request was taken");
	check(run(sensor, chip, 3000, 300, &t, &h) == 1 && chip.singles == 2, "and one more measurement follows");
}

int main() {
	singleShot<MockBus>("SHT31<MockBus>: bare ISensorTransport, asynchronous");
	singleShot<LoopI2C>("SHT31<LoopI2C>: itransport's I2CTransport");

	std::printf("periodic mode\n");
	{
		MockChip chip;
		SHT31<LoopI2C> sensor(kPeriodic, chip);
		float t = NAN, h = NAN;
		int n = run(sensor, chip, 0, 7000, &t, &h);
		check(chip.periodicCmd == 0x2737, "10 measurements a second, high repeatability, sends 0x2737");
		check(n == 3 && chip.fetches == 3 && chip.singles == 0, "a reading fetched every 2 s");
		check(std::fabs(t - 25.0f) < 0.02f && std::fabs(h - 50.0f) < 0.02f, "values correct");
		check(chip.violations == 0, "no command sent that periodic mode does not allow");
	}
	{
		const struct { SHT31_Sample_Rate_t rate; uint16_t cmd; } rates[] = {
			{_05mps_high_Res, 0x2032}, {_05_med_Res, 0x2024}, {_05_low_Res, 0x202F},
			{_1mps_high_Res, 0x2130}, {_1mps_med_Res, 0x2126}, {_1mps_low_Res, 0x212D},
			{_2mps_high_Res, 0x2236}, {_2mps_med_Res, 0x2220}, {_2mps_low_Res, 0x222B},
			{_4mps_high_Res, 0x2334}, {_4mps_med_Res, 0x2322}, {_4mps_low_Res, 0x2329},
			{_10mps_high_Res, 0x2737}, {_10mps_med_Res, 0x2721}, {_10mps_low_Res, 0x272A}};
		bool ok = true;
		for (const auto& r : rates) {
			MockChip chip; SHT31_param_t p = {true, false, r.rate};
			SHT31<LoopI2C> sensor(p, chip);
			run(sensor, chip, 0, 400);
			if (chip.periodicCmd != r.cmd) ok = false;
		}
		check(ok, "each of the 15 sample rates sends its own command");
	}
	{
		MockChip chip; chip.samplePeriod = 5000;		// chip slower than the driver asks
		SHT31<LoopI2C> sensor(kPeriodic, chip);
		const int n = run(sensor, chip, 0, 9000);
		check(n == 1 && chip.nacks >= 2 && chip.resets == 1, "a fetch with nothing new is not an error: no reset, the next reading still arrives");
	}

	std::printf("requests while in periodic mode\n");
	{
		MockChip chip;
		SHT31<LoopI2C> sensor(kPeriodic, chip);
		run(sensor, chip, 0, 500);
		const int at = (int)chip.commands.size();
		sensor.ForceMeasurement();
		const int n = run(sensor, chip, 500, 200);
		check(n == 1 && chip.singles == 1, "ForceMeasurement(): a single measurement within 200 ms");
		check(chip.indexOf(0x3093, at) >= 0 && chip.indexOf(0x3093, at) < chip.indexOf(0x2400, at), "periodic mode is stopped with a break first");
		check(chip.indexOf(0x2737, at) > chip.indexOf(0x2400, at) && chip.periodic, "and started again afterwards");
		check(run(sensor, chip, 700, 4500) == 2, "periodic readings carry on");

		sensor.SetPeriodicMode(_2mps_med_Res);
		run(sensor, chip, 5200, 200);
		check(chip.periodicCmd == 0x2220 && chip.periodic, "SetPeriodicMode(): the new rate reaches the chip");
		sensor.SendBreak();
		run(sensor, chip, 5400, 200);
		check(!chip.periodic && run(sensor, chip, 5600, 5000) == 0, "SendBreak(): periodic mode stays off");
		check(chip.violations == 0 && chip.resets == 1, "all of it without a forbidden command or a reset");
	}

	std::printf("heater\n");
	{
		MockChip chip;
		SHT31<LoopI2C> sensor(kSingle, chip);
		run(sensor, chip, 0, 500);
		check(!sensor.isHeaterEnabled() && chip.count(0x306D) == 0, "off, and no heater command, by default");
		sensor.heater(true);
		run(sensor, chip, 500, 100);
		check(chip.heater && sensor.isHeaterEnabled(), "heater(true): switched on, and the status read back says so");
		check((sensor.readStatus() & 0x2000) != 0, "readStatus() gives that status word");
		sensor.heater(false);
		run(sensor, chip, 600, 100);
		check(!chip.heater && !sensor.isHeaterEnabled(), "heater(false): switched off");
	}
	{
		MockChip chip; SHT31_param_t p = {false, true, _1mps_high_Res};
		SHT31<LoopI2C> sensor(p, chip);
		run(sensor, chip, 0, 500);
		check(chip.heater && sensor.isHeaterEnabled(), "heater_enable in the settings switches it on at start up");
	}

	std::printf("alert limits\n");
	{
		MockChip chip;
		SHT31<LoopI2C> sensor(kPeriodic, chip);
		run(sensor, chip, 0, 500);
		const SHT31_Alert_t high = {60.0f, 58.0f, 80.0f, 79.0f};
		const SHT31_Alert_t low = {-10.0f, -9.0f, 20.0f, 22.0f};
		sensor.setHighAlert(&high);
		sensor.setLowAlert(&low);
		run(sensor, chip, 500, 300);
		check(chip.badAlertCrc == 0 && chip.count(0x611D) == 1 && chip.count(0x6116) == 1 && chip.count(0x6100) == 1 && chip.count(0x610B) == 1,
			  "four limits written, each to its own command, each with a good CRC");
		check(std::fabs(MockChip::alertTemp(chip.alert[0]) - 60.0f) < 0.4f && std::fabs(MockChip::alertHum(chip.alert[0]) - 80.0f) < 0.8f, "high set limit on the chip is 60 C, 80 %RH");
		check(std::fabs(MockChip::alertTemp(chip.alert[1]) - 58.0f) < 0.4f && std::fabs(MockChip::alertHum(chip.alert[1]) - 79.0f) < 0.8f, "high clear limit is 58 C, 79 %RH");
		check(std::fabs(MockChip::alertTemp(chip.alert[3]) - (-10.0f)) < 0.4f && std::fabs(MockChip::alertHum(chip.alert[3]) - 20.0f) < 0.8f, "low set limit is -10 C, 20 %RH");
		check(std::fabs(MockChip::alertTemp(chip.alert[2]) - (-9.0f)) < 0.4f && std::fabs(MockChip::alertHum(chip.alert[2]) - 22.0f) < 0.8f, "low clear limit is -9 C, 22 %RH");
		check(chip.violations == 0 && chip.periodic, "periodic mode stopped for the writes and started again");
		SHT31_Alert_t back = {0, 0, 0, 0};
		sensor.ReadHighAlert(&back);
		check(back.SetTemp == 60.0f && back.ClearHumidity == 79.0f, "ReadHighAlert() hands back the limits that were set");

		check(!sensor.HighTempActive() && !sensor.LowHumidityActive(), "no alert at 25 C, 50 %RH");
		chip.temperature = 65.0f; chip.humidity = 15.0f;
		run(sensor, chip, 800, 2500);
		check(sensor.HighTempActive() && sensor.LowHumidityActive() && !sensor.HighHumidityActive() && !sensor.LowTempActive(), "65 C and 15 %RH raise the high temperature and low humidity alerts");
		chip.temperature = 59.0f; chip.humidity = 21.0f;
		run(sensor, chip, 3300, 2500);
		check(sensor.HighTempActive() && sensor.LowHumidityActive(), "they hold between the set and clear limits");
		chip.temperature = 57.0f; chip.humidity = 23.0f;
		run(sensor, chip, 5800, 2500);
		check(!sensor.HighTempActive() && !sensor.LowHumidityActive(), "and clear past the clear limits");
	}
	{
		MockChip chip; chip.temperature = 120.0f; chip.humidity = 0.5f;
		SHT31<LoopI2C> sensor(kSingle, chip);
		run(sensor, chip, 0, 500);
		check(!sensor.HighTempActive() && !sensor.LowTempActive() && !sensor.HighHumidityActive() && !sensor.LowHumidityActive(), "with no limits set, no alert is ever raised");
	}

	std::printf("reading corrupted on the way back\n");
	{
		MockChip chip; chip.corrupt = true;
		SHT31<LoopI2C> sensor(kSingle, chip);
		check(run(sensor, chip, 0, 3000) == 0 && chip.singles >= 2, "CRC failure: reading rejected, and tried again");
		chip.corrupt = false;
		check(run(sensor, chip, 3000, 2000) >= 1, "next good read is accepted");
	}

	std::printf("device absent, then plugged in\n");
	{
		MockChip chip; chip.present = false;
		SHT31<LoopI2C> sensor(kSingle, chip);
		check(run(sensor, chip, 0, 2000) == 0, "no readings while absent");
		chip.present = true;
		check(run(sensor, chip, 2000, 2000) >= 1, "recovers by itself once the device answers");
	}

	std::printf("operation that never completes\n");
	{
		MockChip chip;
		SHT31<MockBus> sensor(kPeriodic, chip);
		run(sensor, chip, 0, 2100);
		chip.stuck = true;
		run(sensor, chip, 2100, 4000);
		check(sensor.state() == sht31_error_state, "gives up after the bus timeout");
		chip.stuck = false;
		check(run(sensor, chip, 6100, 4000) >= 1, "recovers when the bus comes back");
	}

	std::printf("transport that cannot start an operation\n");
	{
		MockChip chip;
		SHT31<MockBus> sensor(kPeriodic, chip);
		run(sensor, chip, 0, 500);
		size_t before = chip.commands.size();
		chip.refuse = true;
		check(run(sensor, chip, 500, 4000) == 0 && chip.commands.size() == before, "nothing reaches the bus");
		chip.refuse = false;
		check(run(sensor, chip, 4500, 4000) >= 1, "recovers when the bus is free");
	}

	std::printf("tick counter rollover\n");
	{
		MockChip chip;
		SHT31<LoopI2C> sensor(kPeriodic, chip);
		const uint32_t start = 0xFFFFFFFFu - 4500;	// wraps 4.5 s into the run
		check(run(sensor, chip, start, 9000) == 4, "readings keep their 2 s rhythm across the wrap");
	}

	std::printf("xSHT31: sleep times under an OS\n");
	{
		MockChip chip;
		xSHT31<MockBus> sensor(kPeriodic, chip);
		// Like a thread: main(), then advance the clock by however long
		// main() slept, plus the one tick the loop gives up every cycle.
		uint32_t now = 0; int calls = 0, readings = 0; uint32_t longest = 0;
		g_delays.clear();
		while (now < 20000) {
			chip.now = now; g_slept = 0;
			sensor.main(now); ++calls;
			if (sensor.newData()) { float v; sensor.GetTemperature(&v); ++readings; }
			now += g_slept + 1;
		}
		for (uint32_t d : g_delays) if (d > longest) longest = d;
		std::printf("        %d readings in 20 s from %d calls to main(), longest sleep %u ms\n", readings, calls, (unsigned)longest);
		check(readings >= 8, "still fetches on schedule");
		check(longest == 100, "never sleeps longer than 100 ms, so a request is seen promptly");
		check(calls < 400, "wakes only when there is something to do");
	}

	std::printf("\n%s\n", g_failures ? "FAILED" : "all passed");
	return g_failures ? 1 : 0;
}
