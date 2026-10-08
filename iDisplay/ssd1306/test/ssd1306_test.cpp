/*
 * ssd1306_test.cpp
 *
 * Host test for ssd1306<TTransport, H>. No hardware, HAL or RTOS needed.
 * IT is the itransport folder (iTransport/itransport), ID the iDisplay
 * folder:
 *
 *   g++ -std=c++17 -Wall -Wextra -I../inc -Istub -I$IT/inc -I<isensor>/inc -I$ID/inc \
 *       ssd1306_test.cpp $IT/src/BusTransport.cpp $IT/src/I2CTransport.cpp \
 *       $ID/src/MonoCanvas.cpp $ID/src/iTextSurface.cpp $ID/src/Font5x7.cpp -o ssd1306_test
 */

#include <cstdio>
#include <algorithm>
#include <cstring>
#include <initializer_list>
#include <vector>
#include "I2CTransport.h"
#include "Font5x7.h"
#include "xssd1306.h"	// pulls in ssd1306.h. stub/cmsis_os2.h stands in for the RTOS

using namespace SSD1306;

uint32_t g_slept = 0;						// set by the stub osDelay()
std::vector<uint32_t> g_delays;

// ---- simulated SSD1306 ----
// Takes commands and display data the way the chip does: a command
// byte, then as many argument bytes as that command has, and data into
// the RAM at the page and column set, the column moving on after each
// byte (page addressing mode).
struct MockChip {
	uint8_t ram[8][132];
	bool present = true;
	bool stuck = false;						// transfers never complete
	bool refuse = false;					// transport cannot start a transfer
	uint8_t page = 0, col = 0;
	bool on = false, inverted = false;
	uint8_t contrast = 0x7F, mux = 63, comPins = 0x12, chargePump = 0, memMode = 0xFF;
	uint8_t segRemap = 0xA0, comScan = 0xC0;
	int inits = 0;							// DISPLAY_OFF followed by the clock divider: a power-up sequence
	int commandWrites = 0, dataWrites = 0, dataBytes = 0;
	std::vector<uint8_t> commands;
	uint8_t pendingCmd = 0; int argsLeft = 0;

	MockChip() { std::memset(ram, 0xAA, sizeof ram); }

	static int argCount(uint8_t c) {
		switch (c) {
		case 0x81: case 0x8D: case 0xA8: case 0xD3: case 0xD5: case 0xD9: case 0xDA: case 0xDB: case 0x20: return 1;
		case 0x21: case 0x22: return 2;
		default: return 0;
		}
	}
	void command(uint8_t b) {
		commands.push_back(b);
		if (argsLeft > 0) {
			--argsLeft;
			switch (pendingCmd) {
			case 0x81: contrast = b; break;
			case 0x8D: chargePump = b; break;
			case 0xA8: mux = b; break;
			case 0xDA: comPins = b; break;
			case 0x20: memMode = b; break;
			case 0xD5: if (commands.size() >= 3 && commands[commands.size() - 3] == 0xAE) ++inits; break;
			default: break;
			}
			return;
		}
		pendingCmd = b;
		argsLeft = argCount(b);
		if (b == 0xAE) on = false;
		else if (b == 0xAF) on = true;
		else if (b == 0xA6) inverted = false;
		else if (b == 0xA7) inverted = true;
		else if (b == 0xA0 || b == 0xA1) segRemap = b;
		else if (b == 0xC0 || b == 0xC8) comScan = b;
		else if ((b & 0xF8) == 0xB0) page = b & 0x07;
		else if ((b & 0xF0) == 0x00) col = (uint8_t)((col & 0xF0) | (b & 0x0F));
		else if ((b & 0xF0) == 0x10) col = (uint8_t)((col & 0x0F) | ((b & 0x0F) << 4));
	}
	void data(uint8_t b) {
		ram[page][col] = b;
		col = (uint8_t)((col + 1) % 132);
		++dataBytes;
	}
	void transfer(bool isData, const uint8_t* p, uint8_t len) {
		if (isData) ++dataWrites; else ++commandWrites;
		for (uint8_t i = 0; i < len; ++i) { if (isData) data(p[i]); else command(p[i]); }
	}
	bool commandSeen(std::initializer_list<uint8_t> seq) const {
		const std::vector<uint8_t> s(seq);
		if (commands.size() < s.size()) return false;
		for (size_t i = 0; i + s.size() <= commands.size(); ++i)
			if (std::equal(s.begin(), s.end(), commands.begin() + i)) return true;
		return false;
	}
};

// ---- transport 1: a bare ISensorTransport whose transfers take time ----
class MockBus : public ISensorTransport {
public:
	explicit MockBus(MockChip& c) : chip(c) {}
	bool writeReg(uint8_t r, uint8_t v) override { return writeRegs(r, &v, 1); }
	bool writeRegs(uint8_t r, const uint8_t* buf, uint8_t len) override {
		if (busy || chip.refuse || len == 0 || len > kMaxWriteLen) return false;
		failed = !chip.present;
		if (chip.present) {
			if (r == 0x00) chip.transfer(false, buf, len);
			else if (r == 0x40) chip.transfer(true, buf, len);
			else badControl = true;
		}
		busy = true; polls = 1;
		return true;
	}
	bool readRegs(uint8_t, uint8_t*, uint8_t) override { return false; }
	bool writeBytes(const uint8_t*, uint8_t) override { return false; }
	bool readBytes(uint8_t*, uint8_t) override { return false; }
	bool isBusy() const override {
		if (!busy) return false;
		if (chip.stuck) return true;
		if (polls > 0) { --polls; return true; }
		busy = false;
		return false;
	}
	bool lastOpFailed() const override { return failed; }
	bool checkDevice() override { return chip.present; }
	bool badControl = false;
private:
	MockChip& chip;
	mutable bool busy = false; mutable int polls = 0;
	bool failed = false;
};

// ---- transport 2: itransport's real I2CTransport, with the HAL calls simulated ----
static int g_i2cBus;	// stands in for &hi2c1
class LoopI2C : public I2CTransport {
public:
	explicit LoopI2C(MockChip& c) : I2CTransport(&g_i2cBus, SSD1306_I2C_ADDR, nullptr), chip(c) {}
protected:
	bool halMemWrite(uint8_t reg, uint8_t* p, uint16_t size) override {
		if (chip.present) chip.transfer(reg == 0x40, p, (uint8_t)size);
		BusTransport::onTransferComplete(&g_i2cBus, !chip.present);	// what the I2C interrupt does
		return true;
	}
	bool halMemRead(uint8_t, uint8_t*, uint16_t) override { return false; }
	bool halMasterTransmit(uint8_t*, uint16_t) override { return false; }
	bool halMasterReceive(uint8_t*, uint16_t) override { return false; }
	bool halIsDeviceReady(uint32_t, uint32_t) override { return chip.present; }
private:
	MockChip& chip;
};

// ---- transport 3: SPI, where a D/C pin says what the bytes are ----
static bool g_dc = false;
static void setDc(bool data) { g_dc = data; }
class MockSpi : public ISensorTransport {
public:
	explicit MockSpi(MockChip& c) : chip(c) {}
	bool writeReg(uint8_t, uint8_t) override { return false; }
	bool writeRegs(uint8_t, const uint8_t*, uint8_t) override { return false; }
	bool readRegs(uint8_t, uint8_t*, uint8_t) override { return false; }
	bool writeBytes(const uint8_t* buf, uint8_t len) override {
		if (busy) return false;
		chip.transfer(g_dc, buf, len);
		busy = true; polls = 1;
		return true;
	}
	bool readBytes(uint8_t*, uint8_t) override { return false; }
	bool isBusy() const override {
		if (!busy) return false;
		if (polls > 0) { --polls; return true; }
		busy = false;
		return false;
	}
	bool lastOpFailed() const override { return false; }
	bool checkDevice() override { return true; }
private:
	MockChip& chip;
	mutable bool busy = false; mutable int polls = 0;
};

// ---- harness ----
static int g_failures = 0;
static void check(bool ok, const char* what) {
	std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok) ++g_failures;
}

// Runs main() once per ms, as the GUI task does, until the flush is done
// or the display fails. Returns the ms it took.
template <typename TDisplay>
uint32_t flush(TDisplay& d, uint32_t& now, uint32_t limit = 5000) {
	d.requestFlush();
	uint32_t start = now;
	do {
		d.main(now++);
	} while (!d.idle() && !d.failed() && now - start < limit);
	return now - start;
}

template <typename TDisplay>
bool ramMatches(const MockChip& chip, const TDisplay& d, uint8_t pages, uint8_t offset = 0) {
	for (uint8_t p = 0; p < pages; ++p)
		for (uint8_t x = 0; x < 128; ++x)
			if (chip.ram[p][x + offset] != d.buffer()[p * 128 + x]) return false;
	return true;
}

static void drawDemo(idisplay::iTextSurface& s, int n) {
	s.clear();
	s.printAt(0, 0, "Temp: ");
	s.print(21.5f, 1);
	s.printAt(0, 1, "Humidity: ");
	s.print((int32_t)n);
	s.putChar('%');
}

template <typename TTransport>
void normalRun(const char* name) {
	std::printf("%s\n", name);
	MockChip chip;
	ssd1306<TTransport, 64> oled(ssd1306_default_param(64), chip);
	uint32_t now = 0;
	check(!oled.initialized() && oled.text().cols() == 21 && oled.text().rows() == 8, "21 x 8 characters, not yet powered up");

	drawDemo(oled, 40);
	flush(oled, now);
	check(oled.idle() && oled.initialized(), "first flush powers it up and finishes");
	check(chip.inits == 1 && chip.on, "one power-up sequence, display on");
	check(chip.mux == 63 && chip.comPins == 0x12 && chip.chargePump == 0x14 && chip.memMode == 0x02,
		"multiplex 63, COM pins 0x12, charge pump on, page addressing");
	check(chip.contrast == 0xCF && chip.segRemap == 0xA1 && chip.comScan == 0xC8, "contrast 0xCF, normal orientation");
	check(chip.commandWrites == 1 + 8 && chip.dataWrites == 8 * 4 && chip.dataBytes == 1024,
		"then every page: 8 page addresses and 32 data writes of 32 bytes");
	check(ramMatches(chip, oled, 8), "the chip's RAM holds the frame buffer");

	const uint8_t* T = idisplay::fontGlyph('T');
	check(std::memcmp(&chip.ram[0][0], T, 5) == 0 && chip.ram[0][5] == 0, "'T' in the top left cell, then a blank column");

	const int cw = chip.commandWrites, dw = chip.dataWrites;
	drawDemo(oled, 40);
	flush(oled, now);
	check(chip.commandWrites == cw && chip.dataWrites == dw, "cleared and drawn the same: nothing sent");

	drawDemo(oled, 41);
	flush(oled, now);
	check(chip.commandWrites == cw + 1 && chip.dataWrites == dw + 4, "one row changed: that page only");
	check(ramMatches(chip, oled, 8), "and the RAM matches again");

	oled.setContrast(0x10);
	oled.setInverted(true);
	check(!oled.idle(), "a setting waiting to be sent: not idle");
	flush(oled, now);
	check(chip.contrast == 0x10 && chip.inverted, "contrast and inverse sent between flushes");
	check(chip.commandSeen({0x81, 0x10, 0xA7}), "in one transfer: 0x81 0x10 0xA7");
	oled.setDisplayOn(false);
	flush(oled, now);
	check(!chip.on && chip.inits == 1, "display off, without a new power-up");
}

static void failures() {
	std::printf("ssd1306: failures and recovery\n");
	{
		MockChip chip;
		chip.present = false;
		ssd1306<MockBus, 64> oled(ssd1306_default_param(64), chip);
		uint32_t now = 0;
		drawDemo(oled, 1);
		flush(oled, now);
		check(oled.failed() && !oled.initialized(), "no chip: the power-up NACKs and it fails");
		chip.present = true;
		uint32_t t = 0;
		while (!oled.idle() && t++ < 2000) oled.main(now++);
		check(oled.idle() && chip.inits == 1, "chip back: after the backoff it powers up by itself");
		check(ramMatches(chip, oled, 8), "and sends the frame that was asked for");
	}
	{
		MockChip chip;
		ssd1306<MockBus, 64> oled(ssd1306_default_param(64), chip);
		uint32_t now = 0;
		drawDemo(oled, 1);
		flush(oled, now);
		chip.present = false;					// unplugged
		drawDemo(oled, 2);
		flush(oled, now);
		check(oled.failed(), "unplugged mid-run: fails");
		chip.present = true;
		std::memset(chip.ram, 0, sizeof chip.ram);	// it lost power: RAM is gone
		const int dw = chip.dataWrites;
		uint32_t t = 0;
		while (!oled.idle() && t++ < 2000) oled.main(now++);
		check(chip.inits == 2 && chip.dataWrites - dw == 32, "plugged back: power-up again and all 8 pages, not just the changed one");
		check(ramMatches(chip, oled, 8), "the RAM matches");
	}
	{
		MockChip chip;
		ssd1306<MockBus, 64> oled(ssd1306_default_param(64), chip);
		uint32_t now = 0;
		chip.stuck = true;
		const uint32_t took = flush(oled, now);
		check(oled.failed() && took >= 100 && took < 200, "a transfer that never lands: fails at the 100 ms bus timeout");
	}
	{
		MockChip chip;
		ssd1306<MockBus, 64> oled(ssd1306_default_param(64), chip);
		uint32_t now = 0;
		chip.refuse = true;
		const uint32_t took = flush(oled, now);
		check(oled.failed() && took >= 100 && took < 200 && chip.commandWrites == 0, "a bus that never frees: fails at the bus timeout");
	}
}

static void variants() {
	std::printf("ssd1306: 128x32, orientation, offset, SPI, rollover, sleep\n");
	{
		MockChip chip;
		ssd1306<MockBus, 32> oled(ssd1306_default_param(32), chip);
		uint32_t now = 0;
		drawDemo(oled, 5);
		flush(oled, now);
		check(oled.text().rows() == 4 && chip.mux == 31 && chip.comPins == 0x02 && chip.contrast == 0x8F,
			"128x32: 4 rows, multiplex 31, COM pins 0x02, contrast 0x8F");
		check(chip.dataWrites == 16 && ramMatches(chip, oled, 4), "4 pages sent, RAM matches");
	}
	{
		MockChip chip;
		ssd1306_param_t p = ssd1306_default_param(64);
		p.rotate_180 = true;
		p.column_offset = 2;
		ssd1306<MockBus, 64> oled(p, chip);
		uint32_t now = 0;
		drawDemo(oled, 5);
		flush(oled, now);
		check(chip.segRemap == 0xA0 && chip.comScan == 0xC0, "rotate_180: 0xA0 and 0xC0");
		check(ramMatches(chip, oled, 8, 2), "column_offset 2: the frame lands two columns in (SH1106)");
	}
	{
		MockChip chip;
		ssd1306_spi<MockSpi, 64> oled(ssd1306_default_param(64), setDc, chip);
		uint32_t now = 0;
		drawDemo(oled, 5);
		flush(oled, now);
		check(oled.idle() && chip.inits == 1 && chip.dataBytes == 1024, "SPI: D/C low for commands, high for data");
		check(ramMatches(chip, oled, 8), "SPI: RAM matches");
	}
	{
		MockChip chip;
		ssd1306<LoopI2C, 64> oled(ssd1306_default_param(64), chip);
		uint32_t now = 0xFFFFFF00u;				// 256 ms before the 32 bit tick rolls over
		drawDemo(oled, 5);
		flush(oled, now);
		check(oled.idle() && ramMatches(chip, oled, 8), "across the tick rollover");
	}
	{
		MockChip chip;
		xssd1306<MockBus, 64> oled(ssd1306_default_param(64), chip);
		uint32_t now = 0;
		g_slept = 0; g_delays.clear();
		drawDemo(oled, 5);
		flush(oled, now);
		bool allOne = !g_delays.empty();
		for (uint32_t d : g_delays) allOne = allOne && d == 1;
		check(allOne, "xssd1306: osDelay(1) while each transfer is in flight");
		chip.present = false;
		drawDemo(oled, 6);
		flush(oled, now);
		g_delays.clear();
		oled.main(now);
		check(oled.failed() && !g_delays.empty() && g_delays.back() <= 500, "xssd1306: sleeps out the backoff in the error state");
	}
}

int main() {
	normalRun<MockBus>("ssd1306<MockBus>: bare ISensorTransport, asynchronous");
	normalRun<LoopI2C>("ssd1306<LoopI2C>: the real I2CTransport");
	failures();
	variants();
	std::printf("%s (%d failed)\n", g_failures ? "FAILED" : "all passed", g_failures);
	return g_failures ? 1 : 0;
}
