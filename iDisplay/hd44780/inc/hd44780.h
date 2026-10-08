/*
 * hd44780.h
 *
 *  HD44780 character LCD (16x2, 20x4, ...) behind an I2C port expander,
 *  non-blocking state machine. Works with the two usual backpacks:
 *
 *    PCF8574 / PCF8574A   the common blue "I2C LCD" board (0x27 / 0x3F)
 *    MCP23008             Adafruit's I2C/SPI character LCD backpack (0x20)
 *
 *  The bus is chosen by the template argument. hd44780<TTransport, COLS,
 *  ROWS> inherits from TTransport, which must be an ISensorTransport
 *  (itransport/inc/ISensorTransport.h):
 *
 *      hd44780<Stm32HalI2CTransport, 20, 4> lcd(hd44780_pcf8574_param(), &hi2c1, HD44780_PCF8574_ADDR, i2c1Mutex);
 *
 *  Everything after param goes to the transport's own constructor.
 *  This file includes no HAL and no RTOS header.
 *
 *  It is also an iTextSurface, with the characters held in RAM, and an
 *  iDisplayDevice, which is what the GUI (iDisplay/gui) drives. Drawing
 *  only changes the buffer; requestFlush() sends what changed.
 *
 *  The LCD runs in 4-bit mode. Each nibble is three writes to the
 *  expander: the data with E low, with E high, with E low again, so RS
 *  and the data are steady before E rises and E falls on steady data.
 *  A character is six expander bytes, and a transfer carries five
 *  characters (30 of ISensorTransport::kMaxWriteLen's 32 bytes).
 *
 *  What it does: drives the expander's pins low (PCF8574) or makes them
 *  outputs (MCP23008), waits 50 ms from the first main() for the LCD's
 *  power-up, puts it into 4-bit mode with the datasheet's three 0x3
 *  nibbles and a 0x2, configures it and clears it. Each flush then sends,
 *  row by row, only the run of characters that changed since they were
 *  last sent. Backlight, display on/off and the eight custom characters
 *  (CGRAM) are sent between flushes. A failure restarts all of that,
 *  custom characters and contents included.
 *
 *  Inverse cannot be shown and is ignored. The cell a text field is
 *  editing (iTextSurface::showEditCursor) gets the LCD's blinking cursor
 *  instead. The 5x7 font's degree sign (0x7F) is sent as the LCD's own
 *  (0xDF in the A00 ROM). Characters 1 to 7 (and 8 for slot 0) show the
 *  custom characters; give them with defineChar(). In the A00 ROM '\'
 *  is a yen sign and '~' a right arrow.
 *
 *  Timing: the HD44780 needs about 40 us between instructions. A byte
 *  here takes at least 20 us on a 400 kHz bus and a character six of
 *  them, so the bus is the delay; don't run it faster than 400 kHz.
 *  Clear is the one slow instruction (1.52 ms) and is followed by a
 *  wait. The busy flag is never read; R/W stays low.
 *
 *  Not done: the direct 4/8-bit GPIO wiring and the 74HC595 (SPI) side
 *  of Adafruit's backpack, and 5x10 fonts. Not yet run against an LCD;
 *  the sequence and timings are from the Hitachi HD44780U datasheet.
 */

#ifndef HD44780_H_
#define HD44780_H_

#include <stdint.h>
#include <string.h>
#include <type_traits>
#include <utility>
#include "ISensorTransport.h"
#include "SensorStateMachine.h"
#include "iTextSurface.h"
#include "iDisplayDevice.h"

namespace HD44780 {

#define HD44780_PCF8574_ADDR 0x27		// 7 bit, A0-A2 high (the usual board)
#define HD44780_PCF8574A_ADDR 0x3F
#define HD44780_MCP23008_ADDR 0x20		// Adafruit backpack, no jumpers

// Instructions
#define HD44780_CLEAR 0x01
#define HD44780_ENTRY_MODE 0x04
#define HD44780_ENTRY_INCREMENT 0x02
#define HD44780_DISPLAY_CONTROL 0x08
#define HD44780_DISPLAY_ON 0x04
#define HD44780_CURSOR_ON 0x02
#define HD44780_BLINK_ON 0x01
#define HD44780_FUNCTION_SET 0x20
#define HD44780_FUNCTION_8BIT 0x10
#define HD44780_FUNCTION_2LINE 0x08
#define HD44780_SET_CGRAM 0x40
#define HD44780_SET_DDRAM 0x80

#define HD44780_DEGREE 0xDF				// in the A00 character ROM
#define HD44780_NO_PIN 0xFF

// MCP23008 registers
#define MCP23008_IODIR 0x00
#define MCP23008_IOCON 0x05
#define MCP23008_GPIO 0x09
#define MCP23008_IOCON_SEQOP 0x20		// the address does not move on: every byte goes to GPIO

typedef enum hd44780_expander_t{
	hd44780_pcf8574 = 0,				// no registers: each byte written is the port
	hd44780_mcp23008					// bytes written to GPIO, sequential addressing off
}hd44780_expander_t;

// Which expander pin (0..7) drives which LCD pin.
typedef struct hd44780_param_t{
	hd44780_expander_t expander;
	uint8_t rs, rw, en;					// rw: HD44780_NO_PIN when tied to ground
	uint8_t d4, d5, d6, d7;
	uint8_t backlight;					// HD44780_NO_PIN when there is none
	bool backlight_active_low;
}hd44780_param_t;

// The usual PCF8574 board: P0 RS, P1 RW, P2 E, P3 backlight, P4-P7 D4-D7.
inline hd44780_param_t hd44780_pcf8574_param() {
	hd44780_param_t p = {};
	p.expander = hd44780_pcf8574;
	p.rs = 0; p.rw = 1; p.en = 2; p.backlight = 3;
	p.d4 = 4; p.d5 = 5; p.d6 = 6; p.d7 = 7;
	p.backlight_active_low = false;
	return p;
}

// Adafruit's backpack (MCP23008): GP1 RS, GP2 E, GP3-GP6 D4-D7, GP7
// backlight, RW to ground.
inline hd44780_param_t hd44780_adafruit_backpack_param() {
	hd44780_param_t p = {};
	p.expander = hd44780_mcp23008;
	p.rs = 1; p.rw = HD44780_NO_PIN; p.en = 2; p.backlight = 7;
	p.d4 = 3; p.d5 = 4; p.d6 = 5; p.d7 = 6;
	p.backlight_active_low = false;
	return p;
}

typedef enum hd44780_state_t{
	hd44780_start = 0,					// note when power-up began
	hd44780_expander_setup,				// pins to outputs, all low
	hd44780_wait_expander_setup,
	hd44780_power_wait,					// 50 ms from start
	hd44780_reset_nibble,				// 0x3, 0x3, 0x3, then 0x2: into 4-bit mode
	hd44780_wait_reset_nibble,
	hd44780_reset_delay,
	hd44780_configure,					// function set, display control, entry mode, clear
	hd44780_clear_delay,				// clear takes 1.52 ms
	hd44780_idle,
	hd44780_backlight,					// one expander byte, no E pulse
	hd44780_wait_backlight,
	hd44780_flush_row,					// the next row that changed
	hd44780_flush_cursor,				// then the edit cursor
	hd44780_send,						// the instructions and characters queued, five at a time
	hd44780_wait_send,
	hd44780_error
}hd44780_state_t;

template <typename TTransport, uint8_t COLS = 16, uint8_t ROWS = 2>
class hd44780 : public SensorStateMachine<TTransport, hd44780_state_t>,
				public idisplay::iTextSurface,
				public idisplay::iDisplayDevice {
	static_assert(std::is_base_of<ISensorTransport, TTransport>::value,
		"hd44780 needs an ISensorTransport (I2CTransport, ...)");
	static_assert(ROWS >= 1 && ROWS <= 4, "1 to 4 rows");
	static_assert(COLS >= 1 && COLS <= 40, "at most 40 columns");
	static_assert(ROWS <= 2 || COLS <= 20, "a 4 row LCD has at most 20 columns");

	using Base = SensorStateMachine<TTransport, hd44780_state_t>;

public:
	static constexpr uint8_t kCells = COLS * ROWS;
	static constexpr uint8_t kBytesPerNibble = 3;
	static constexpr uint8_t kBytesPerSymbol = 2 * kBytesPerNibble;
	static constexpr uint8_t kSymbolsPerTransfer = ISensorTransport::kMaxWriteLen / kBytesPerSymbol;
	static constexpr uint8_t kMaxSymbols = COLS + 1;		// an address and a row; >= 9 for a custom character
	static constexpr uint32_t kBusTimeoutMs = 100;
	static constexpr uint32_t kErrorBackoffMs = 500;
	// Waits, in ticks of a 1 ms clock. A wait of n ticks can be as short
	// as n - 1 ms (it may start late in a tick), so each is the
	// datasheet's time rounded up, plus one.
	static constexpr uint32_t kPowerUpMs = 50;			// datasheet: 40 ms after VCC reaches 2.7 V
	static constexpr uint32_t kFirstResetMs = 6;		// datasheet: 4.1 ms
	static constexpr uint32_t kResetMs = 2;				// datasheet: 100 us
	static constexpr uint32_t kClearMs = 3;				// datasheet: 1.52 ms

	template <typename... TArgs>
	explicit hd44780(const hd44780_param_t& param, TArgs&&... transportArgs);

	// iTextSurface
	void clear() override;
	void showEditCursor(uint8_t col, uint8_t row) override;

	// iDisplayDevice
	idisplay::iTextSurface& text() override { return *this; }
	void requestFlush() override { flush_wanted = true; }
	void main(uint32_t nowMs) override;
	bool idle() const override;
	bool failed() const override { return this->_state == hd44780_error; }

	// Sent at the next main() between flushes, and again after a failure.
	void setBacklight(bool on);
	void setDisplayOn(bool on);
	// Custom character slot 0..7, shown by characters 1..7 (and 8 for
	// slot 0). rows: 8 rows top to bottom, the low 5 bits of each.
	void defineChar(uint8_t slot, const uint8_t rows[8]);

	bool initialized() const { return is_initialized; }
	// What the cell holds now, for tests and for screens that want to know.
	char cell(uint8_t col, uint8_t row) const { return (col < COLS && row < ROWS) ? chars[row * COLS + col] : ' '; }

protected:
	void drawCell(uint8_t col, uint8_t row, char c, bool inverse) override;
	void onFail() override;

private:
	static uint8_t rowAddress(uint8_t row) {
		return row == 0 ? 0x00 : row == 1 ? 0x40 : row == 2 ? COLS : (uint8_t)(0x40 + COLS);
	}
	static uint8_t toLcd(char c);

	uint8_t port(uint8_t nibble, bool rs, bool en) const;
	uint8_t encodeNibble(uint8_t* out, uint8_t nibble, bool rs) const;
	bool writePort(const uint8_t* buf, uint8_t len);
	uint8_t displayControl(bool cursor) const;

	void queueClear() { sym_count = 0; sym_pos = 0; }
	void queue(uint8_t b, bool rs);
	void sendQueue(hd44780_state_t after, uint32_t nowMs);

	hd44780_param_t param;

	char chars[kCells];					// as drawn
	char sent[kCells];					// as the LCD has it

	bool is_initialized = false;
	bool flush_wanted = false;
	bool backlight_on = true, backlight_pending = false;
	bool display_on = true, display_pending = false;
	uint8_t cgram_defined = 0, cgram_pending = 0;
	uint8_t cgram[8][8];

	// The edit cursor, as asked for and as last sent.
	bool edit_visible = false;
	uint8_t edit_addr = 0;
	bool cursor_sent_visible = false;
	uint8_t cursor_sent_addr = 0;
	bool address_moved = true;			// the LCD's address counter is not where the cursor was sent

	bool started = false;
	uint32_t power_start = 0;
	uint8_t setup_step = 0, reset_step = 0;
	uint8_t flush_row = 0;

	// The queue for hd44780_send: instruction or character bytes, with RS.
	uint8_t sym[kMaxSymbols < 9 ? 9 : kMaxSymbols];
	bool sym_rs[kMaxSymbols < 9 ? 9 : kMaxSymbols];
	uint8_t sym_count = 0, sym_pos = 0, sym_chunk = 0;
	hd44780_state_t after_send = hd44780_idle;

	uint8_t tx[ISensorTransport::kMaxWriteLen];
};

} /* namespace HD44780 */

#include "../src/hd44780.tpp"

#endif /* HD44780_H_ */
