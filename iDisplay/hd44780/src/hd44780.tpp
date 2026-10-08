/*
 * hd44780.tpp
 *
 *  Member definitions of hd44780<TTransport, COLS, ROWS>. Included at the
 *  end of inc/hd44780.h; not compiled on its own.
 */

#include "../inc/hd44780.h"

namespace HD44780 {

template <typename TTransport, uint8_t COLS, uint8_t ROWS>
template <typename... TArgs>
hd44780<TTransport, COLS, ROWS>::hd44780(const hd44780_param_t& p, TArgs&&... transportArgs)
	: Base(hd44780_start, hd44780_error, kBusTimeoutMs, std::forward<TArgs>(transportArgs)...),
	  idisplay::iTextSurface(COLS, ROWS),
	  param(p) {
	memset(chars, ' ', sizeof chars);
	memset(sent, ' ', sizeof sent);
	memset(cgram, 0, sizeof cgram);
}

// ---- drawing ----

template <typename TTransport, uint8_t COLS, uint8_t ROWS>
void hd44780<TTransport, COLS, ROWS>::clear() {
	memset(chars, ' ', sizeof chars);
	edit_visible = false;
	homeCursor();
}

template <typename TTransport, uint8_t COLS, uint8_t ROWS>
void hd44780<TTransport, COLS, ROWS>::drawCell(uint8_t col, uint8_t row, char c, bool inverse) {
	(void)inverse;		// an HD44780 has no inverse
	chars[row * COLS + col] = c;
}

template <typename TTransport, uint8_t COLS, uint8_t ROWS>
void hd44780<TTransport, COLS, ROWS>::showEditCursor(uint8_t col, uint8_t row) {
	if (col >= COLS || row >= ROWS) return;
	edit_visible = true;
	edit_addr = (uint8_t)(rowAddress(row) + col);
}

template <typename TTransport, uint8_t COLS, uint8_t ROWS>
uint8_t hd44780<TTransport, COLS, ROWS>::toLcd(char c) {
	const uint8_t u = (uint8_t)c;
	if (u == 0x7F) return HD44780_DEGREE;
	if (u == 0) return ' ';			// never sent as slot 0: a cleared cell is a space
	return u;
}

// ---- settings ----

template <typename TTransport, uint8_t COLS, uint8_t ROWS>
bool hd44780<TTransport, COLS, ROWS>::idle() const {
	return this->_state == hd44780_idle && !flush_wanted
		&& !backlight_pending && !display_pending && cgram_pending == 0;
}

template <typename TTransport, uint8_t COLS, uint8_t ROWS>
void hd44780<TTransport, COLS, ROWS>::setBacklight(bool on) {
	backlight_on = on;
	backlight_pending = true;
}

template <typename TTransport, uint8_t COLS, uint8_t ROWS>
void hd44780<TTransport, COLS, ROWS>::setDisplayOn(bool on) {
	display_on = on;
	display_pending = true;
}

template <typename TTransport, uint8_t COLS, uint8_t ROWS>
void hd44780<TTransport, COLS, ROWS>::defineChar(uint8_t slot, const uint8_t rows[8]) {
	if (slot > 7 || rows == nullptr) return;
	for (uint8_t i = 0; i < 8; ++i) cgram[slot][i] = (uint8_t)(rows[i] & 0x1F);
	cgram_defined |= (uint8_t)(1u << slot);
	cgram_pending |= (uint8_t)(1u << slot);
}

template <typename TTransport, uint8_t COLS, uint8_t ROWS>
void hd44780<TTransport, COLS, ROWS>::onFail() {
	// The LCD may have lost power, or be half way through a byte: start
	// again from the reset nibbles, which bring it back from either. Its
	// RAM is gone, so the custom characters go again and the contents
	// are sent against a cleared screen.
	is_initialized = false;
	started = false;
	flush_wanted = true;				// the start again clears the screen: send it all
	cgram_pending = cgram_defined;
	display_pending = false;			// part of the configuration
	backlight_pending = false;			// part of every expander byte
}

// ---- the expander ----

// One expander byte: the nibble on D4-D7, RS, E, the backlight. RW is
// always low (write).
template <typename TTransport, uint8_t COLS, uint8_t ROWS>
uint8_t hd44780<TTransport, COLS, ROWS>::port(uint8_t nibble, bool rs, bool en) const {
	uint8_t v = 0;
	if (nibble & 0x1) v |= (uint8_t)(1u << param.d4);
	if (nibble & 0x2) v |= (uint8_t)(1u << param.d5);
	if (nibble & 0x4) v |= (uint8_t)(1u << param.d6);
	if (nibble & 0x8) v |= (uint8_t)(1u << param.d7);
	if (rs) v |= (uint8_t)(1u << param.rs);
	if (en) v |= (uint8_t)(1u << param.en);
	if (param.backlight != HD44780_NO_PIN && backlight_on != param.backlight_active_low)
		v |= (uint8_t)(1u << param.backlight);
	return v;
}

// E low, E high, E low: RS and data are steady before E rises (tAS), and
// the LCD takes the nibble as E falls.
template <typename TTransport, uint8_t COLS, uint8_t ROWS>
uint8_t hd44780<TTransport, COLS, ROWS>::encodeNibble(uint8_t* out, uint8_t nibble, bool rs) const {
	out[0] = port(nibble, rs, false);
	out[1] = port(nibble, rs, true);
	out[2] = port(nibble, rs, false);
	return kBytesPerNibble;
}

template <typename TTransport, uint8_t COLS, uint8_t ROWS>
bool hd44780<TTransport, COLS, ROWS>::writePort(const uint8_t* buf, uint8_t len) {
	if (param.expander == hd44780_mcp23008) return this->writeRegs(MCP23008_GPIO, buf, len);
	return this->writeBytes(buf, len);
}

template <typename TTransport, uint8_t COLS, uint8_t ROWS>
uint8_t hd44780<TTransport, COLS, ROWS>::displayControl(bool cursor) const {
	return (uint8_t)(HD44780_DISPLAY_CONTROL | (display_on ? HD44780_DISPLAY_ON : 0) | (cursor ? HD44780_BLINK_ON : 0));
}

template <typename TTransport, uint8_t COLS, uint8_t ROWS>
void hd44780<TTransport, COLS, ROWS>::queue(uint8_t b, bool rs) {
	if (sym_count >= sizeof sym) return;
	sym[sym_count] = b;
	sym_rs[sym_count] = rs;
	++sym_count;
}

template <typename TTransport, uint8_t COLS, uint8_t ROWS>
void hd44780<TTransport, COLS, ROWS>::sendQueue(hd44780_state_t after, uint32_t nowMs) {
	sym_pos = 0;
	after_send = after;
	this->enter(sym_count ? hd44780_send : after, nowMs);
}

// ---- the state machine ----

template <typename TTransport, uint8_t COLS, uint8_t ROWS>
void hd44780<TTransport, COLS, ROWS>::main(uint32_t nowMs) {
	switch (this->_state) {
	case hd44780_start:
		if (!started) {
			started = true;
			power_start = nowMs;
		}
		setup_step = 0;
		this->enter(hd44780_expander_setup, nowMs);
		break;

	case hd44780_expander_setup: {
		// MCP23008: sequential addressing off (so a run of bytes all goes
		// to GPIO), all pins outputs, then low. PCF8574: its pins come up
		// high, E included; drive them low.
		bool started_op;
		if (param.expander == hd44780_mcp23008 && setup_step == 0) {
			started_op = this->writeReg(MCP23008_IOCON, MCP23008_IOCON_SEQOP);
		} else if (param.expander == hd44780_mcp23008 && setup_step == 1) {
			started_op = this->writeReg(MCP23008_IODIR, 0x00);
		} else {
			tx[0] = port(0, false, false);
			started_op = writePort(tx, 1);
		}
		this->issued(started_op, hd44780_wait_expander_setup, nowMs);
		break;
	}

	case hd44780_wait_expander_setup:
		if (this->landed(nowMs)) {
			const uint8_t steps = param.expander == hd44780_mcp23008 ? 3 : 1;
			if (++setup_step < steps) this->enter(hd44780_expander_setup, nowMs);
			else this->enter(hd44780_power_wait, nowMs);
		}
		break;

	case hd44780_power_wait:
		if ((uint32_t)(nowMs - power_start) >= kPowerUpMs) {
			reset_step = 0;
			this->enter(hd44780_reset_nibble, nowMs);
		} else {
			this->sleepRemaining(power_start, nowMs, kPowerUpMs);
		}
		break;

	case hd44780_reset_nibble:
		// Datasheet figure 24: 0x3 three times (8-bit function set, sent
		// as one nibble while the LCD may be in either mode), then 0x2.
		encodeNibble(tx, reset_step < 3 ? 0x3 : 0x2, false);
		this->issued(writePort(tx, kBytesPerNibble), hd44780_wait_reset_nibble, nowMs);
		break;

	case hd44780_wait_reset_nibble:
		if (this->landed(nowMs)) this->enter(hd44780_reset_delay, nowMs);
		break;

	case hd44780_reset_delay: {
		// > 4.1 ms after the first, > 100 us after the others.
		const uint32_t wait = reset_step == 0 ? kFirstResetMs : kResetMs;
		if (!this->elapsed(nowMs, wait)) {
			this->sleepRemaining(nowMs, wait);
			break;
		}
		if (++reset_step < 4) this->enter(hd44780_reset_nibble, nowMs);
		else this->enter(hd44780_configure, nowMs);
		break;
	}

	case hd44780_configure:
		// Clear goes last: it is the slow one, and clear_delay waits it out.
		queueClear();
		queue((uint8_t)(HD44780_FUNCTION_SET | (ROWS > 1 ? HD44780_FUNCTION_2LINE : 0)), false);
		queue(displayControl(false), false);
		queue((uint8_t)(HD44780_ENTRY_MODE | HD44780_ENTRY_INCREMENT), false);
		queue(HD44780_CLEAR, false);
		sendQueue(hd44780_clear_delay, nowMs);
		break;

	case hd44780_clear_delay:
		if (!this->elapsed(nowMs, kClearMs)) {
			this->sleepRemaining(nowMs, kClearMs);
			break;
		}
		memset(sent, ' ', sizeof sent);	// what clear left
		cursor_sent_visible = false;
		address_moved = true;
		is_initialized = true;
		this->enter(hd44780_idle, nowMs);
		break;

	case hd44780_idle:
		if (backlight_pending) {
			backlight_pending = false;
			this->enter(hd44780_backlight, nowMs);
		} else if (cgram_pending) {
			uint8_t slot = 0;
			while (!(cgram_pending & (1u << slot))) ++slot;
			cgram_pending &= (uint8_t)~(1u << slot);
			queueClear();
			queue((uint8_t)(HD44780_SET_CGRAM | (slot << 3)), false);
			for (uint8_t i = 0; i < 8; ++i) queue(cgram[slot][i], true);
			// The address counter is in CGRAM now; a cursor on show has
			// to be put back, which a flush does.
			address_moved = true;
			if (cursor_sent_visible) flush_wanted = true;
			sendQueue(hd44780_idle, nowMs);
		} else if (display_pending) {
			display_pending = false;
			queueClear();
			queue(displayControl(cursor_sent_visible), false);
			sendQueue(hd44780_idle, nowMs);
		} else if (flush_wanted) {
			flush_wanted = false;
			flush_row = 0;
			this->enter(hd44780_flush_row, nowMs);
		}
		break;

	case hd44780_backlight:
		tx[0] = port(0, false, false);
		this->issued(writePort(tx, 1), hd44780_wait_backlight, nowMs);
		break;

	case hd44780_wait_backlight:
		if (this->landed(nowMs)) this->enter(hd44780_idle, nowMs);
		break;

	case hd44780_flush_row: {
		// The next row with a change, and the run from its first changed
		// character to its last: one address and the characters between.
		uint8_t first = 0, last = 0;
		bool found = false;
		while (flush_row < ROWS && !found) {
			const char* now = &chars[flush_row * COLS];
			const char* was = &sent[flush_row * COLS];
			for (uint8_t c = 0; c < COLS; ++c) {
				if (toLcd(now[c]) != toLcd(was[c])) {
					if (!found) first = c;
					last = c;
					found = true;
				}
			}
			if (!found) ++flush_row;
		}
		if (!found) {
			this->enter(hd44780_flush_cursor, nowMs);
			break;
		}
		queueClear();
		queue((uint8_t)(HD44780_SET_DDRAM | (rowAddress(flush_row) + first)), false);
		for (uint8_t c = first; c <= last; ++c) {
			const uint8_t i = (uint8_t)(flush_row * COLS + c);
			queue(toLcd(chars[i]), true);
			sent[i] = chars[i];
		}
		address_moved = true;
		++flush_row;
		sendQueue(hd44780_flush_row, nowMs);
		break;
	}

	case hd44780_flush_cursor:
		queueClear();
		if (edit_visible) {
			if (!cursor_sent_visible || address_moved || cursor_sent_addr != edit_addr) {
				queue((uint8_t)(HD44780_SET_DDRAM | edit_addr), false);
				if (!cursor_sent_visible) queue(displayControl(true), false);
			}
			cursor_sent_visible = true;
			cursor_sent_addr = edit_addr;
			address_moved = false;
		} else if (cursor_sent_visible) {
			queue(displayControl(false), false);
			cursor_sent_visible = false;
		}
		sendQueue(hd44780_idle, nowMs);
		break;

	case hd44780_send: {
		sym_chunk = (uint8_t)(sym_count - sym_pos);
		if (sym_chunk > kSymbolsPerTransfer) sym_chunk = kSymbolsPerTransfer;
		uint8_t n = 0;
		for (uint8_t i = 0; i < sym_chunk; ++i) {
			const uint8_t b = sym[sym_pos + i];
			const bool rs = sym_rs[sym_pos + i];
			n += encodeNibble(&tx[n], (uint8_t)(b >> 4), rs);
			n += encodeNibble(&tx[n], (uint8_t)(b & 0x0F), rs);
		}
		// The transport copies the bytes, so tx is free once this returns.
		this->issued(writePort(tx, n), hd44780_wait_send, nowMs);
		break;
	}

	case hd44780_wait_send:
		if (this->landed(nowMs)) {
			sym_pos = (uint8_t)(sym_pos + sym_chunk);
			this->enter(sym_pos < sym_count ? hd44780_send : after_send, nowMs);
		}
		break;

	case hd44780_error:
		if (this->errorCleared(nowMs, kErrorBackoffMs)) this->enter(hd44780_start, nowMs);
		break;

	default:
		this->enter(hd44780_start, nowMs);
		break;
	}
}

} /* namespace HD44780 */
