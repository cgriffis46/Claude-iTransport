/*
 * hd44780_test.cpp
 *
 * Host test for hd44780<TTransport, COLS, ROWS>. No hardware, HAL or RTOS
 * needed. IT is the itransport folder (iTransport/itransport), ID the
 * iDisplay folder:
 *
 *   g++ -std=c++17 -Wall -Wextra -I../inc -Istub -I$IT/inc -I<isensor>/inc -I$ID/inc \
 *       hd44780_test.cpp $IT/src/BusTransport.cpp $IT/src/I2CTransport.cpp \
 *       $ID/src/iTextSurface.cpp -o hd44780_test
 */

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "I2CTransport.h"
#include "xhd44780.h"	// pulls in hd44780.h. stub/cmsis_os2.h stands in for the RTOS

using namespace HD44780;

uint32_t g_slept = 0;						// set by the stub osDelay()
std::vector<uint32_t> g_delays;

// ---- simulated HD44780 ----
// Sees only what is on its pins, through the expander: RS, E, D4-D7.
// Starts in 8-bit mode as at power-up, takes a nibble as E falls, and
// checks the timing rules the driver has to keep: 40 ms from power-up
// to the first instruction, 4.1 ms after the first reset nibble, and
// nothing while a clear is running.
struct MockLcd {
	uint32_t now = 0;						// the test's clock; power came on at 0
	hd44780_param_t pins;
	uint8_t ddram[128];
	uint8_t cgram[64];
	bool mode8 = true, haveHigh = false, cg = false;
	uint8_t high = 0, ac = 0, lines = 1;
	bool displayOn = false, cursor = false, blink = false, increment = false;
	bool backlight = false;
	uint32_t busyUntil = 0;
	int instructions = 0, dataWrites = 0, clears = 0, violations = 0, setupViolations = 0;
	std::vector<uint8_t> log;				// every instruction, in order
	std::vector<uint8_t> resetLog;			// the 8-bit-mode latches
	uint8_t prev = 0;
	bool havePrev = false;

	explicit MockLcd(const hd44780_param_t& p) : pins(p) {
		std::memset(ddram, 0xA5, sizeof ddram);	// power-up garbage
		std::memset(cgram, 0, sizeof cgram);
	}
	bool bit(uint8_t v, uint8_t pin) const { return pin != HD44780_NO_PIN && ((v >> pin) & 1); }
	uint8_t nibble(uint8_t v) const {
		return (uint8_t)(bit(v, pins.d4) | (bit(v, pins.d5) << 1) | (bit(v, pins.d6) << 2) | (bit(v, pins.d7) << 3));
	}
	void port(uint8_t v) {
		backlight = bit(v, pins.backlight) != pins.backlight_active_low;
		if (havePrev) {
			const bool eWas = bit(prev, pins.en), eNow = bit(v, pins.en);
			const uint8_t mask = (uint8_t)~((1u << pins.en) | (pins.backlight != HD44780_NO_PIN ? (1u << pins.backlight) : 0));
			if (!eWas && eNow && ((prev ^ v) & mask)) ++setupViolations;	// RS/data changed as E rose
			if (eWas && !eNow) {
				if ((prev ^ v) & mask) ++setupViolations;					// changed as E fell
				latch(nibble(prev), bit(prev, pins.rs));
			}
		}
		prev = v;
		havePrev = true;
	}
	void latch(uint8_t n, bool rs) {
		if (mode8) {
			resetLog.push_back(n);
			execute((uint8_t)(n << 4), rs);	// D0-D3 are not wired: 0
			return;
		}
		if (!haveHigh) { high = n; haveHigh = true; return; }
		haveHigh = false;
		execute((uint8_t)((high << 4) | n), rs);
	}
	void execute(uint8_t b, bool rs) {
		if (now < 40 || (int32_t)(now - busyUntil) < 0) ++violations;
		busyUntil = now;
		if (rs) {
			++dataWrites;
			if (cg) { cgram[ac & 0x3F] = b; ac = (uint8_t)((ac + 1) & 0x3F); }
			else { ddram[ac] = b; advance(); }
			return;
		}
		++instructions;
		log.push_back(b);
		if (b & 0x80) { ac = b & 0x7F; cg = false; }
		else if (b & 0x40) { ac = b & 0x3F; cg = true; }
		else if (b & 0x20) {
			const bool eight = b & 0x10;
			if (mode8 && eight) {
				// The reset sequence: 4.1 ms after the first, 100 us after the others.
				busyUntil = now + (resetLog.size() == 1 ? 5 : 1);
			}
			mode8 = eight;
			if (!eight) lines = (b & 0x08) ? 2 : 1;
		}
		else if (b & 0x10) {}
		else if (b & 0x08) { displayOn = b & 0x04; cursor = b & 0x02; blink = b & 0x01; }
		else if (b & 0x04) { increment = b & 0x02; }
		else if (b & 0x02) { ac = 0; cg = false; busyUntil = now + 2; }
		else if (b & 0x01) {
			std::memset(ddram, ' ', sizeof ddram);
			ac = 0; cg = false; ++clears;
			busyUntil = now + 2;
		}
	}
	void advance() {
		ac = (uint8_t)(ac + 1);
		if (lines == 2) { if (ac == 0x28) ac = 0x40; else if (ac == 0x68) ac = 0x00; }
		else if (ac == 0x50) ac = 0;
	}
	std::string row(uint8_t r, uint8_t cols, uint8_t rowsTotal) const {
		(void)rowsTotal;
		const uint8_t off = r == 0 ? 0x00 : r == 1 ? 0x40 : r == 2 ? cols : (uint8_t)(0x40 + cols);
		std::string s;
		for (uint8_t c = 0; c < cols; ++c) s += (char)ddram[off + c];
		while (!s.empty() && s.back() == ' ') s.pop_back();
		return s;
	}
	uint8_t cursorAddr() const { return ac; }
};

// ---- expander 1: PCF8574, a bare ISensorTransport ----
struct Bus {
	bool present = true, stuck = false, refuse = false;
	int transfers = 0, maxLen = 0, bytes = 0;
};

class MockPcf : public ISensorTransport {
public:
	MockPcf(MockLcd& l, Bus& b) : lcd(l), bus(b) {}
	bool writeReg(uint8_t, uint8_t) override { wrongCall = true; return false; }
	bool writeRegs(uint8_t, const uint8_t*, uint8_t) override { wrongCall = true; return false; }
	bool readRegs(uint8_t, uint8_t*, uint8_t) override { return false; }
	bool writeBytes(const uint8_t* buf, uint8_t len) override {
		if (busy || bus.refuse || len == 0 || len > kMaxWriteLen) return false;
		failed = !bus.present;
		if (bus.present) for (uint8_t i = 0; i < len; ++i) lcd.port(buf[i]);
		++bus.transfers; bus.bytes += len; if (len > bus.maxLen) bus.maxLen = len;
		busy = true; polls = 1;
		return true;
	}
	bool readBytes(uint8_t*, uint8_t) override { return false; }
	bool isBusy() const override {
		if (!busy) return false;
		if (bus.stuck) return true;
		if (polls > 0) { --polls; return true; }
		busy = false;
		return false;
	}
	bool lastOpFailed() const override { return failed; }
	bool checkDevice() override { return bus.present; }
	bool wrongCall = false;
private:
	MockLcd& lcd;
	Bus& bus;
	mutable bool busy = false; mutable int polls = 0;
	bool failed = false;
};

// ---- expander 2: MCP23008 behind itransport's real I2CTransport ----
// Registers as the datasheet has them: with IOCON.SEQOP clear the
// address moves on after each byte, so a run of bytes to GPIO would land
// in OLAT and beyond; with it set, every byte goes to GPIO. Pins drive
// the LCD only once IODIR makes them outputs.
static int g_i2cBus;	// stands in for &hi2c1
class LoopMcp : public I2CTransport {
public:
	LoopMcp(MockLcd& l, Bus& b) : I2CTransport(&g_i2cBus, HD44780_MCP23008_ADDR, nullptr), lcd(l), bus(b) {}
	uint8_t iodir = 0xFF, iocon = 0;
	int gpioWrites = 0;
protected:
	bool halMemWrite(uint8_t reg, uint8_t* p, uint16_t size) override {
		if (bus.refuse) return false;
		++bus.transfers; bus.bytes += size; if (size > bus.maxLen) bus.maxLen = size;
		if (bus.present) {
			uint8_t r = reg;
			for (uint16_t i = 0; i < size; ++i) {
				if (r == MCP23008_IODIR) iodir = p[i];
				else if (r == MCP23008_IOCON) iocon = p[i];
				else if (r == MCP23008_GPIO) { ++gpioWrites; if (iodir == 0x00) lcd.port(p[i]); }
				if (!(iocon & MCP23008_IOCON_SEQOP)) r = (uint8_t)((r + 1) % 11);
			}
		}
		BusTransport::onTransferComplete(&g_i2cBus, !bus.present);	// what the I2C interrupt does
		return true;
	}
	bool halMemRead(uint8_t, uint8_t*, uint16_t) override { return false; }
	bool halMasterTransmit(uint8_t*, uint16_t) override { return false; }
	bool halMasterReceive(uint8_t*, uint16_t) override { return false; }
	bool halIsDeviceReady(uint32_t, uint32_t) override { return bus.present; }
private:
	MockLcd& lcd;
	Bus& bus;
};

// ---- harness ----
static int g_failures = 0;
static void check(bool ok, const char* what) {
	std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok) ++g_failures;
}

// Runs main() four times per ms, a loop much faster than the waits it
// has to keep (a 1 ms step per call would hide a missing 1.52 ms clear
// wait), until the flush is done or the display fails. Returns the ms.
static const int kCallsPerMs = 4;

template <typename TLcd>
uint32_t flush(TLcd& d, MockLcd& lcd, uint32_t limit = 5000) {
	d.requestFlush();
	uint32_t start = lcd.now;
	for (int i = 0; ; ++i) {
		d.main(lcd.now);
		if (i % kCallsPerMs == kCallsPerMs - 1) ++lcd.now;
		if (d.idle() || d.failed() || lcd.now - start >= limit) break;
	}
	return lcd.now - start;
}

template <typename TLcd>
void runUntilIdle(TLcd& d, MockLcd& lcd, uint32_t limit = 5000) {
	for (uint32_t i = 0; !d.idle() && i < limit * kCallsPerMs; ++i) {
		d.main(lcd.now);
		if (i % kCallsPerMs == kCallsPerMs - 1) ++lcd.now;
	}
}

static void pcf20x4() {
	std::printf("hd44780<MockPcf, 20, 4>: PCF8574 board\n");
	const hd44780_param_t p = hd44780_pcf8574_param();
	MockLcd lcd(p);
	Bus bus;
	hd44780<MockPcf, 20, 4> d(p, lcd, bus);
	check(d.cols() == 20 && d.rows() == 4 && !d.initialized(), "20 x 4, not yet started");

	d.printAt(0, 0, "Temp: "); d.print(21.5f, 1); d.putChar(idisplay::kDegreeChar);
	d.printAt(0, 1, "Humidity: 40%");
	d.printAt(0, 2, "Row three");
	d.printAt(0, 3, "Row four, clipped at twenty");
	flush(d, lcd);
	check(d.idle() && d.initialized(), "first flush starts it up and finishes");
	check(lcd.violations == 0, "no instruction early: 40 ms power-up, 4.1 ms after reset, clear waited out");
	check(lcd.setupViolations == 0, "RS and data steady around every E edge");
	check(lcd.resetLog.size() == 4 && lcd.resetLog[0] == 3 && lcd.resetLog[1] == 3 && lcd.resetLog[2] == 3 && lcd.resetLog[3] == 2,
		"reset: 0x3, 0x3, 0x3 in 8-bit mode, then 0x2");
	check(!lcd.mode8 && lcd.lines == 2 && lcd.displayOn && !lcd.cursor && !lcd.blink && lcd.increment && lcd.clears == 1,
		"4-bit, 2 lines, display on, no cursor, increment, cleared");
	check(lcd.backlight, "backlight on");
	check(lcd.row(0, 20, 4) == "Temp: 21.5\xDF" && lcd.row(1, 20, 4) == "Humidity: 40%", "rows 1 and 2, the degree sign as 0xDF");
	check(lcd.row(2, 20, 4) == "Row three" && lcd.row(3, 20, 4) == "Row four, clipped at",
		"rows 3 and 4 at 0x14 and 0x54, clipped at 20");
	check(bus.maxLen <= 32, "no transfer over 32 bytes");

	const int tx = bus.transfers, writes = lcd.dataWrites;
	d.clear();
	d.printAt(0, 0, "Temp: "); d.print(21.5f, 1); d.putChar(idisplay::kDegreeChar);
	d.printAt(0, 1, "Humidity: 40%");
	d.printAt(0, 2, "Row three");
	d.printAt(0, 3, "Row four, clipped at twenty");
	flush(d, lcd);
	check(bus.transfers == tx, "cleared and drawn the same: nothing sent");

	d.printAt(10, 1, "41");
	flush(d, lcd);
	check(lcd.dataWrites == writes + 1 && lcd.row(1, 20, 4) == "Humidity: 41%", "one character changed: one character sent");
	check(bus.transfers == tx + 1, "the address and the character: one transfer");

	d.setBacklight(false);
	flush(d, lcd);
	check(!lcd.backlight && lcd.violations == 0, "backlight off");
	d.setDisplayOn(false);
	flush(d, lcd);
	check(!lcd.displayOn, "display off");
	d.setDisplayOn(true);
	d.setBacklight(true);
	flush(d, lcd);
	check(lcd.displayOn && lcd.backlight, "and back on");

	d.showEditCursor(4, 2);
	flush(d, lcd);
	check(lcd.blink && lcd.cursorAddr() == 0x14 + 4, "edit cursor: blinking, on column 4 of row 3");
	const int tx2 = bus.transfers;
	d.showEditCursor(4, 2);
	flush(d, lcd);
	check(bus.transfers == tx2, "same place: nothing sent");
	d.printAt(0, 0, "X");
	d.showEditCursor(5, 2);
	flush(d, lcd);
	check(lcd.row(0, 20, 4)[0] == 'X' && lcd.blink && lcd.cursorAddr() == 0x14 + 5, "a character written: the cursor is put back after it");
	d.clear();
	d.printAt(0, 0, "Temp");
	flush(d, lcd);
	check(!lcd.blink && lcd.row(0, 20, 4) == "Temp" && lcd.row(1, 20, 4).empty(), "cleared: cursor off, the rest blanked");
	d.showEditCursor(30, 0);
	flush(d, lcd);
	check(!lcd.blink, "a mark off the screen is ignored");

	const uint8_t bar[8] = {0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F};
	d.defineChar(1, bar);
	d.printAt(0, 3, "\x01\x01");
	flush(d, lcd);
	check(std::memcmp(&lcd.cgram[8], bar, 8) == 0, "custom character 1 in CGRAM");
	check(lcd.ddram[0x54] == 1 && lcd.ddram[0x55] == 1, "and shown by character 1");
	check(lcd.violations == 0 && lcd.setupViolations == 0, "timing and setup kept all along");
}

static void failures() {
	std::printf("hd44780: failures and recovery\n");
	{
		const hd44780_param_t p = hd44780_pcf8574_param();
		MockLcd lcd(p);
		Bus bus;
		bus.present = false;
		hd44780<MockPcf, 16, 2> d(p, lcd, bus);
		d.printAt(0, 0, "Hello");
		flush(d, lcd);
		check(d.failed() && !d.initialized(), "no backpack: the first write NACKs and it fails");
		bus.present = true;
		runUntilIdle(d, lcd);
		check(d.idle() && lcd.row(0, 16, 2) == "Hello" && lcd.violations == 0, "backpack back: starts up by itself and shows the frame");
	}
	{
		const hd44780_param_t p = hd44780_pcf8574_param();
		MockLcd lcd(p);
		Bus bus;
		hd44780<MockPcf, 16, 2> d(p, lcd, bus);
		const uint8_t deg[8] = {6, 9, 9, 6, 0, 0, 0, 0};
		d.defineChar(2, deg);
		d.printAt(0, 0, "Hello");
		d.printAt(0, 1, "World");
		flush(d, lcd);
		bus.present = false;
		d.printAt(0, 0, "Jello");
		flush(d, lcd);
		check(d.failed(), "unplugged mid-run: fails");
		// Power lost: a new LCD, in 8-bit mode with garbage in it.
		MockLcd fresh(p);
		fresh.now = lcd.now;
		lcd = fresh;
		bus.present = true;
		runUntilIdle(d, lcd);
		check(d.idle() && lcd.row(0, 16, 2) == "Jello" && lcd.row(1, 16, 2) == "World", "plugged back: everything sent again, not just the change");
		check(std::memcmp(&lcd.cgram[16], deg, 8) == 0, "custom characters too");
		check(lcd.resetLog.size() == 4 && lcd.violations == 0, "after the full reset, power-up wait included");
	}
	{
		const hd44780_param_t p = hd44780_pcf8574_param();
		MockLcd lcd(p);
		Bus bus;
		hd44780<MockPcf, 16, 2> d(p, lcd, bus);
		bus.stuck = true;
		const uint32_t took = flush(d, lcd);
		check(d.failed() && took >= 100 && took < 200, "a transfer that never lands: fails at the 100 ms bus timeout");
	}
	{
		const hd44780_param_t p = hd44780_pcf8574_param();
		MockLcd lcd(p);
		Bus bus;
		hd44780<MockPcf, 16, 2> d(p, lcd, bus);
		bus.refuse = true;
		const uint32_t took = flush(d, lcd);
		check(d.failed() && took >= 100 && took < 200 && bus.transfers == 0, "a bus that never frees: fails at the bus timeout");
	}
}

static void mcp16x2() {
	std::printf("hd44780<LoopMcp, 16, 2>: Adafruit MCP23008 backpack, real I2CTransport\n");
	const hd44780_param_t p = hd44780_adafruit_backpack_param();
	MockLcd lcd(p);
	Bus bus;
	hd44780<LoopMcp, 16, 2> d(p, lcd, bus);
	d.printAt(0, 0, "Adafruit");
	d.printAt(0, 1, "backpack");
	flush(d, lcd);
	check(d.idle() && lcd.row(0, 16, 2) == "Adafruit" && lcd.row(1, 16, 2) == "backpack", "text on both rows");
	check(lcd.violations == 0 && lcd.setupViolations == 0, "timing and setup kept");
	check(bus.maxLen <= 32, "no transfer over 32 bytes");
}

static void sizes() {
	std::printf("hd44780: 16x1, 40x2, rollover, sleep\n");
	{
		const hd44780_param_t p = hd44780_pcf8574_param();
		MockLcd lcd(p);
		Bus bus;
		hd44780<MockPcf, 16, 1> d(p, lcd, bus);
		d.print("One line");
		flush(d, lcd);
		check(lcd.lines == 1 && lcd.row(0, 16, 1) == "One line", "16x1: function set with one line");
	}
	{
		const hd44780_param_t p = hd44780_pcf8574_param();
		MockLcd lcd(p);
		Bus bus;
		hd44780<MockPcf, 40, 2> d(p, lcd, bus);
		d.printAt(0, 1, "0123456789012345678901234567890123456789");
		flush(d, lcd);
		check(lcd.row(1, 40, 2) == "0123456789012345678901234567890123456789", "40x2: a full 40 character row");
	}
	{
		const hd44780_param_t p = hd44780_pcf8574_param();
		MockLcd lcd(p);
		lcd.now = 0xFFFFFFE0u;				// 32 ms before the tick rolls over
		lcd.busyUntil = lcd.now;
		Bus bus;
		hd44780<MockPcf, 16, 2> d(p, lcd, bus);
		d.print("Rollover");
		const uint32_t start = lcd.now;
		d.requestFlush();
		uint32_t firstInstr = 0;
		for (uint32_t i = 0; i < 1000 && !d.idle(); ++i) {
			const int before = (int)lcd.resetLog.size();
			d.main(lcd.now);
			if (before == 0 && !lcd.resetLog.empty()) firstInstr = lcd.now - start;
			++lcd.now;
		}
		check(d.idle() && lcd.row(0, 16, 2) == "Rollover" && firstInstr >= 50, "across the tick rollover, the 50 ms power-up still waited");
	}
	{
		const hd44780_param_t p = hd44780_pcf8574_param();
		MockLcd lcd(p);
		Bus bus;
		xhd44780<MockPcf, 16, 2> d(p, lcd, bus);
		g_delays.clear();
		d.print("x");
		flush(d, lcd);
		bool sawPowerWait = false;
		for (uint32_t s : g_delays) if (s >= 40) sawPowerWait = true;
		check(sawPowerWait && !g_delays.empty(), "xhd44780: osDelay through the power-up wait and transfers");
	}
}

int main() {
	pcf20x4();
	failures();
	mcp16x2();
	sizes();
	std::printf("%s (%d failed)\n", g_failures ? "FAILED" : "all passed", g_failures);
	return g_failures ? 1 : 0;
}
