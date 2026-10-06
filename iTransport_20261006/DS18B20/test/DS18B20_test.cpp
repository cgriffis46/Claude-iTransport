/*
 * DS18B20_test.cpp
 *
 * Host test for ds18b20<TTransport>. No hardware, HAL or RTOS needed:
 *
 *   g++ -std=c++17 -Wall -Wextra -I../inc -Istub \
 *       -I<safeTransport>/itransport/inc -I<safeTransport>/isensor/inc DS18B20_test.cpp \
 *       <safeTransport>/itransport/src/BusTransport.cpp \
 *       <safeTransport>/itransport/src/OneWireUartTransport.cpp -o DS18B20_test
 *
 * Runs the driver on two transports: a bare iTransportOneWire, and
 * itransport's real OneWireUartTransport with the UART calls faked.
 * Both talk to the same simulated DS18B20.
 */

#include <cstdio>
#include <cmath>
#include <cstring>
#include "OneWireUartTransport.h"
#include "xDS18B20.h"	// pulls in DS18B20.h. stub/cmsis_os2.h stands in for the RTOS

using namespace DS18B20;

uint32_t g_slept = 0;						// set by the stub osDelay()
std::vector<uint32_t> g_delays;

static uint8_t crc8(const uint8_t* p, int n) {
	uint8_t crc = 0;
	for (int i = 0; i < n; ++i) {
		uint8_t b = p[i];
		for (int k = 0; k < 8; ++k) {
			const uint8_t mix = (crc ^ b) & 0x01;
			crc >>= 1;
			if (mix) crc ^= 0x8C;
			b >>= 1;
		}
	}
	return crc;
}

// ---- simulated DS18B20, at the level of whole bytes ----
struct SimChip {
	uint32_t now = 0;						// the test's clock
	bool present = true;
	bool stuck = false;						// operations never complete
	bool refuse = false;					// transport cannot start an operation
	bool corrupt = false;					// one bit of the scratchpad flipped on the way back
	bool allZero = false;					// line held low during the read
	int16_t ambient = 0x0191;				// what a conversion will measure: 25.0625 C
	uint8_t config = 0x7F;					// 12 bit
	uint32_t conversionTime = 750;

	int16_t latched = 0x0550;				// scratchpad temperature. 85 C at power up
	bool converting = false; uint32_t convertStart = 0;
	enum { Idle, RomCommand, FunctionCommand, Sending } state = Idle;
	uint8_t pad[9]; int sendIdx = 0;
	int resets = 0, conversions = 0, reads = 0;

	bool reset() {							// returns presence
		++resets;
		if (!present) return false;
		state = RomCommand;
		return true;
	}
	void write(uint8_t b) {
		if (!present) return;
		if (state == RomCommand) { state = (b == 0xCC) ? FunctionCommand : Idle; return; }
		if (state != FunctionCommand) return;
		if (b == 0x44) { ++conversions; converting = true; convertStart = now; state = Idle; }
		else if (b == 0xBE) {
			if (converting && (now - convertStart) >= conversionTime) { latched = ambient; converting = false; }
			pad[0] = latched & 0xFF; pad[1] = (latched >> 8) & 0xFF;
			pad[2] = 0x4B; pad[3] = 0x46; pad[4] = config; pad[5] = 0xFF; pad[6] = 0x0C; pad[7] = 0x10;
			pad[8] = crc8(pad, 8);
			if (corrupt) pad[2] ^= 0x04;
			if (allZero) std::memset(pad, 0, 9);
			++reads; sendIdx = 0; state = Sending;
		} else state = Idle;
	}
	uint8_t read() {
		if (!present || state != Sending) return 0xFF;	// released line reads as 1s
		const uint8_t b = pad[sendIdx];
		if (++sendIdx >= 9) state = Idle;
		return b;
	}
};

// ---- transport 1: a bare iTransportOneWire whose operations take time ----
class MockWire : public iTransportOneWire {
public:
	explicit MockWire(SimChip& c) : chip(c) {}
	bool reset() override {
		if (busy || chip.refuse) return false;
		presence_ = chip.reset();
		return begin(nullptr, 0);
	}
	bool writeBytes(const uint8_t* buf, uint8_t len) override {
		if (busy || chip.refuse) return false;
		for (uint8_t i = 0; i < len; ++i) chip.write(buf[i]);
		return begin(nullptr, 0);
	}
	bool readBytes(uint8_t* buf, uint8_t len) override {
		if (busy || chip.refuse) return false;
		return begin(buf, len);
	}
	bool isBusy() const override {
		if (!busy) return false;
		if (chip.stuck) return true;
		if (polls > 0) { --polls; return true; }
		for (uint8_t i = 0; i < n; ++i) dest[i] = chip.read();	// lands only now
		busy = false;
		return false;
	}
	bool lastOpFailed() const override { return false; }
	bool presence() const override { return presence_; }
private:
	bool begin(uint8_t* d, uint8_t len) { dest = d; n = len; busy = true; polls = 1; return true; }
	SimChip& chip;
	mutable bool busy = false; mutable int polls = 0;
	uint8_t* dest = nullptr; uint8_t n = 0; bool presence_ = false;
};

// ---- transport 2: itransport's real OneWireUartTransport, with the UART calls faked ----
// Turns the UART characters back into what they mean on the wire,
// passes that to the same simulated chip, and answers as the line would.
static int g_uart;	// stands in for &huart1
class LoopOneWire : public OneWireUartTransport {
public:
	explicit LoopOneWire(SimChip& c) : OneWireUartTransport(&g_uart, nullptr), chip(c) {}
protected:
	bool halSetBaud(uint32_t b) override { baud = b; return true; }
	bool halReceive(uint8_t* r, uint16_t) override { rx = r; return true; }
	bool halTransmit(uint8_t* tx, uint16_t count) override {
		if (chip.refuse) return false;
		for (uint16_t i = 0; i < count; ++i) rx[i] = slot(tx[i]);
		if (!chip.stuck) {											// what the two UART interrupts do
			OneWireUartTransport::onUartRxComplete(&g_uart);
			OneWireUartTransport::onUartTxComplete(&g_uart);
		}
		return true;
	}
	void halAbort() override {}
private:
	uint8_t slot(uint8_t tx) {
		if (baud == 9600) { bits = 0; cur = 0; reading = false; return chip.reset() ? 0xE0 : 0xF0; }
		if (bits == 0 && chip.present && chip.state == SimChip::Sending) {	// chip has a byte to send
			cur = chip.read();
			reading = true;
		}
		if (reading) {												// read slot
			const bool one = (cur >> bits) & 1;
			if (++bits == 8) { bits = 0; cur = 0; reading = false; }
			return one ? 0xFF : 0xFC;								// a 0 holds the line low a little longer
		}
		if (tx == 0xFF) cur |= static_cast<uint8_t>(1u << bits);	// write slot
		if (++bits == 8) { chip.write(cur); cur = 0; bits = 0; }
		return tx;
	}
	SimChip& chip;
	uint8_t* rx = nullptr; uint32_t baud = 0; uint8_t cur = 0; int bits = 0; bool reading = false;
};

// ---- harness ----
static int g_failures = 0;
static void check(bool ok, const char* what) {
	std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok) ++g_failures;
}

// Runs main() once per ms, like a bare metal loop. Returns readings seen.
template <typename TSensor>
int run(TSensor& s, SimChip& chip, uint32_t start, uint32_t ms, float* last = nullptr, float* first = nullptr) {
	int readings = 0;
	for (uint32_t i = 0; i < ms; ++i) {
		chip.now = start + i;
		s.main(chip.now);
		if (s.newData()) {
			float t; s.getTemp(&t);
			if (readings == 0 && first) *first = t;
			if (last) *last = t;
			++readings;
		}
	}
	return readings;
}

template <typename TTransport>
void normalRun(const char* name) {
	std::printf("%s\n", name);
	SimChip chip;
	ds18b20<TTransport> sensor(chip);
	float t = 0; sensor.getTemp(&t);
	check(std::isnan(t), "NAN before the first reading");
	float first = NAN, last = NAN;
	int n = run(sensor, chip, 0, 4000, &last, &first);
	std::printf("        first = %.4f C   last = %.4f C   (%d readings in 4 s)\n", first, last, n);
	check(first == 25.0625f, "first reading is the conversion result, not the 85 C power up value");
	check(last == 25.0625f && n == 3, "one reading per period");
	check(chip.conversions == chip.reads && chip.resets == 2 * chip.reads, "each reading: reset, CC 44, reset, CC BE, read");

	chip.ambient = static_cast<int16_t>(0xFF5E);
	run(sensor, chip, 4000, 2000, &last);
	check(last == -10.125f, "below zero: 0xFF5E reads as -10.125 C");
	chip.ambient = static_cast<int16_t>(0xFC90);
	run(sensor, chip, 6000, 2000, &last);
	check(last == -55.0f, "bottom of range: -55 C");
	chip.ambient = 0x07D0;
	run(sensor, chip, 8000, 2000, &last);
	check(last == 125.0f, "top of range: +125 C");
}

int main() {
	normalRun<MockWire>("ds18b20<MockWire>: bare iTransportOneWire, asynchronous");
	normalRun<LoopOneWire>("ds18b20<LoopOneWire>: itransport's OneWireUartTransport");

	std::printf("resolution below 12 bit\n");
	{
		SimChip chip; chip.config = 0x1F; chip.ambient = 0x0197;	// 9 bit, with junk in the undefined bits
		ds18b20<LoopOneWire> sensor(chip);
		float t = NAN; run(sensor, chip, 0, 2500, &t);
		check(t == 25.0f, "undefined low bits are masked off");
	}

	std::printf("device absent, then plugged in\n");
	{
		SimChip chip; chip.present = false;
		ds18b20<LoopOneWire> sensor(chip);
		check(run(sensor, chip, 0, 5000) == 0, "no readings while absent");
		check(chip.resets >= 3 && chip.resets <= 6, "keeps looking, about once a period");
		chip.present = true;
		float t = NAN;
		check(run(sensor, chip, 5000, 4000, &t) >= 1 && t == 25.0625f, "recovers by itself once the device answers");
	}

	std::printf("device unplugged during the conversion\n");
	{
		SimChip chip;
		ds18b20<LoopOneWire> sensor(chip);
		run(sensor, chip, 0, 1200);				// conversion under way
		chip.present = false;
		check(run(sensor, chip, 1200, 3000) == 0, "no reading made up from an empty line");
	}

	std::printf("scratchpad corrupted on the way back\n");
	{
		SimChip chip; chip.corrupt = true;
		ds18b20<LoopOneWire> sensor(chip);
		check(run(sensor, chip, 0, 4000) == 0 && chip.reads >= 2, "CRC failure: reading rejected");
		chip.corrupt = false;
		float t = NAN;
		check(run(sensor, chip, 4000, 3000, &t) >= 1 && t == 25.0625f, "next good read is accepted");
	}
	{
		SimChip chip; chip.allZero = true;
		ds18b20<MockWire> sensor(chip);
		check(run(sensor, chip, 0, 4000) == 0 && chip.reads >= 2, "all zeros: rejected, though its CRC is also zero");
	}

	std::printf("operation that never completes\n");
	{
		SimChip chip;
		ds18b20<MockWire> sensor(chip);
		run(sensor, chip, 0, 900);
		chip.stuck = true;
		run(sensor, chip, 900, 2000);
		check(sensor.state() == DS18B20_err_state, "gives up after the bus timeout");
		chip.stuck = false;
		check(run(sensor, chip, 2900, 4000) >= 1, "recovers when the bus comes back");
	}

	std::printf("transport that cannot start an operation\n");
	{
		SimChip chip;
		ds18b20<LoopOneWire> sensor(chip);
		run(sensor, chip, 0, 2000);
		const int before = chip.resets;
		chip.refuse = true;
		check(run(sensor, chip, 2000, 3000) == 0 && chip.resets == before, "nothing reaches the bus");
		chip.refuse = false;
		check(run(sensor, chip, 5000, 4000) >= 1, "recovers when the bus is free");
	}

	std::printf("tick counter rollover\n");
	{
		SimChip chip;
		ds18b20<LoopOneWire> sensor(chip);
		const uint32_t start = 0xFFFFFFFFu - 2500;	// wraps 2.5 s into the run
		float t = NAN;
		check(run(sensor, chip, start, 6000, &t) == 5 && t == 25.0625f, "readings keep their 1 s rhythm across the wrap");
	}

	std::printf("xds18b20: sleep times under an OS\n");
	{
		SimChip chip;
		xds18b20<LoopOneWire> sensor(chip);
		// Like a thread: main(), then advance the clock by however long
		// main() slept, plus the one tick the loop gives up every cycle.
		uint32_t now = 0; int calls = 0, readings = 0;
		g_delays.clear();
		while (now < 10000) {
			chip.now = now; g_slept = 0;
			sensor.main(now); ++calls;
			if (sensor.newData()) { float v; sensor.getTemp(&v); ++readings; }
			now += g_slept + 1;
		}
		bool conversion = false, rest = false;
		for (uint32_t d : g_delays) { if (d >= 740 && d <= 750) conversion = true; if (d >= 200 && d <= 250) rest = true; }
		std::printf("        %d readings in 10 s from %d calls to main()\n", readings, calls);
		check(conversion, "sleeps the whole conversion time");
		check(rest, "then sleeps what is left of the period");
		check(readings >= 8, "still measures on schedule");
		check(calls < 200, "wakes only when there is something to do");
	}

	std::printf("\n%s\n", g_failures ? "FAILED" : "all passed");
	return g_failures ? 1 : 0;
}
