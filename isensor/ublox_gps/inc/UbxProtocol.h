/*
 * UbxProtocol.h
 *
 *  The parts of u-blox's binary UBX protocol the driver needs: reading
 *  frames (for ACK-ACK and ACK-NAK) and building the configuration
 *  messages that choose which NMEA sentences are sent and how often.
 *  Pure logic, header-only, no heap.
 *
 *  Frame: B5 62, class, id, length (2 bytes, little-endian), payload,
 *  CK_A, CK_B. The checksum is an 8-bit Fletcher sum over class, id,
 *  length and payload.
 *
 *  Two ways to configure, by receiver generation:
 *   - Legacy (u-blox 6, 7, 8; NEO-6M, NEO-M8N...): UBX-CFG-MSG (06 01)
 *     sets one NMEA sentence's rate, UBX-CFG-RATE (06 08) the
 *     measurement period. Each is answered with an ACK.
 *   - ValSet (u-blox 9 and 10, which dropped most legacy CFG
 *     messages; NEO-M9N, MAX-M10S...): one UBX-CFG-VALSET (06 8A) with
 *     key/value pairs, written to RAM only, answered with one ACK.
 *  Generation 9 still accepts some legacy messages; generation 10 does
 *  not, so use ValSet for both.
 *
 *  The class/id numbers, the frame and checksum, the CFG-VALSET layout
 *  and layer bits, and the key IDs below were taken from Zephyr's u-blox
 *  modem headers and SparkFun's u-blox GNSS library, which agree. They
 *  were not checked against u-blox's own interface descriptions, which
 *  could not be downloaded here. Not run against a receiver.
 */

#ifndef UBX_PROTOCOL_H_
#define UBX_PROTOCOL_H_

#include <stddef.h>
#include <stdint.h>

namespace ubx {

const uint8_t kSync1 = 0xB5;
const uint8_t kSync2 = 0x62;

const uint8_t kClassAck = 0x05;
const uint8_t kIdAckNak = 0x00;
const uint8_t kIdAckAck = 0x01;

const uint8_t kClassCfg   = 0x06;
const uint8_t kIdCfgMsg    = 0x01;
const uint8_t kIdCfgRate   = 0x08;
const uint8_t kIdCfgValSet = 0x8A;

// The NMEA "class" in CFG-MSG.
const uint8_t kClassNmea = 0xF0;

// CFG-VALSET layers.
const uint8_t kLayerRam   = 0x01;
const uint8_t kLayerBbr   = 0x02;
const uint8_t kLayerFlash = 0x04;

// CFG-VALSET keys. The top nibble of byte 3 is the value's size:
// 0x2 one byte, 0x3 two bytes.
const uint32_t kKeyRateMeas = 0x30210001;   // U2, ms between measurements
const uint32_t kKeyRateNav  = 0x30210002;   // U2, measurements per solution

// The NMEA sentences that can be chosen, with their CFG-MSG id and
// their CFG-VALSET key for output on UART1.
enum class Nmea : uint8_t { GGA, GLL, GSA, GSV, RMC, VTG, ZDA, Count };

inline uint8_t nmeaMsgId(Nmea s) {
	static const uint8_t ids[] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x08};
	return ids[(uint8_t)s];
}
inline uint32_t nmeaUart1Key(Nmea s) {
	static const uint32_t keys[] = {0x209100bb, 0x209100ca, 0x209100c0, 0x209100c5,
	                                0x209100ac, 0x209100b1, 0x209100d9};
	return keys[(uint8_t)s];
}

// Fletcher checksum over n bytes (class to the end of the payload).
inline void checksum(const uint8_t* p, size_t n, uint8_t* ckA, uint8_t* ckB) {
	uint8_t a = 0, b = 0;
	for (size_t i = 0; i < n; ++i) { a = (uint8_t)(a + p[i]); b = (uint8_t)(b + a); }
	*ckA = a;
	*ckB = b;
}

// Builds a frame into out (which needs len + 8 bytes). Returns its size.
inline size_t frame(uint8_t* out, uint8_t cls, uint8_t id, const uint8_t* payload, uint16_t len) {
	out[0] = kSync1;
	out[1] = kSync2;
	out[2] = cls;
	out[3] = id;
	out[4] = (uint8_t)len;
	out[5] = (uint8_t)(len >> 8);
	for (uint16_t i = 0; i < len; ++i) out[6 + i] = payload[i];
	checksum(out + 2, (size_t)len + 4, &out[6 + len], &out[7 + len]);
	return (size_t)len + 8;
}

// UBX-CFG-MSG for one NMEA sentence: (class F0, its id, rate on the
// port this arrives on). rate 0 turns it off, 1 sends it every solution.
inline size_t cfgMsg(uint8_t* out, Nmea s, uint8_t rate) {
	const uint8_t p[3] = {kClassNmea, nmeaMsgId(s), rate};
	return frame(out, kClassCfg, kIdCfgMsg, p, 3);
}

// UBX-CFG-RATE: measurement period in ms, solutions per measurement,
// time reference (0 UTC, 1 GPS).
inline size_t cfgRate(uint8_t* out, uint16_t measMs, uint16_t navRate, uint16_t timeRef) {
	const uint8_t p[6] = {(uint8_t)measMs, (uint8_t)(measMs >> 8), (uint8_t)navRate,
	                      (uint8_t)(navRate >> 8), (uint8_t)timeRef, (uint8_t)(timeRef >> 8)};
	return frame(out, kClassCfg, kIdCfgRate, p, 6);
}

// Collects key/value pairs into a UBX-CFG-VALSET payload.
class ValSet {
public:
	static const uint16_t kMaxPayload = 64;

	explicit ValSet(uint8_t layers) : _len(0), _ok(true) {
		_p[_len++] = 0;        // version: no transaction
		_p[_len++] = layers;
		_p[_len++] = 0;        // reserved
		_p[_len++] = 0;
	}
	void addU1(uint32_t key, uint8_t v) {
		if (!room(5)) return;
		addKey(key);
		_p[_len++] = v;
	}
	void addU2(uint32_t key, uint16_t v) {
		if (!room(6)) return;
		addKey(key);
		_p[_len++] = (uint8_t)v;
		_p[_len++] = (uint8_t)(v >> 8);
	}
	bool ok() const { return _ok; }          // false if a pair did not fit
	uint16_t length() const { return _len; }
	// Builds the frame into out (length() + 8 bytes). Returns its size.
	size_t build(uint8_t* out) const { return frame(out, kClassCfg, kIdCfgValSet, _p, _len); }

private:
	bool room(uint16_t n) {
		if (_len + n > kMaxPayload) { _ok = false; return false; }
		return true;
	}
	void addKey(uint32_t key) {
		for (int i = 0; i < 4; ++i) _p[_len++] = (uint8_t)(key >> (8 * i));
	}
	uint8_t  _p[kMaxPayload];
	uint16_t _len;
	bool     _ok;
};

// Reads UBX frames from a byte stream, one byte at a time. Keeps the
// first kMaxPayload bytes of each payload; a longer frame is still
// checked and reported, with truncated() set.
class Parser {
public:
	static const uint16_t kMaxPayload = 32;

	enum class Result : uint8_t {
		NotMine,     // not part of a UBX frame: give the byte to the NMEA parser
		Consumed,    // part of a frame still coming in
		Frame,       // a frame with a good checksum is complete
		Bad          // a frame ended with a bad checksum
	};

	Parser() : _state(Idle), _len(0), _got(0), _cls(0), _id(0), _ckA(0), _ckB(0), _frames(0), _errors(0) {}

	// True while in the middle of a frame (after B5 62).
	bool inFrame() const { return _state != Idle && _state != Sync2; }

	Result feed(uint8_t b) {
		switch (_state) {
		case Idle:
			if (b != kSync1) return Result::NotMine;
			_state = Sync2;
			return Result::Consumed;
		case Sync2:
			if (b == kSync2) { _state = Class; _a = 0; _b = 0; return Result::Consumed; }
			_state = Idle;
			// B5 then something else: B5 is not text, so nothing is
			// lost by not handing it back; this byte is looked at anew.
			return feed(b);
		case Class:  _cls = b; sum(b); _state = Id; return Result::Consumed;
		case Id:     _id = b;  sum(b); _state = Len1; return Result::Consumed;
		case Len1:   _len = b; sum(b); _state = Len2; return Result::Consumed;
		case Len2:
			_len = (uint16_t)(_len | (b << 8));
			sum(b);
			_got = 0;
			_state = _len ? Payload : CkA;
			return Result::Consumed;
		case Payload:
			if (_got < kMaxPayload) _payload[_got] = b;
			++_got;
			sum(b);
			if (_got == _len) _state = CkA;
			return Result::Consumed;
		case CkA: _ckA = b; _state = CkB; return Result::Consumed;
		case CkB:
			_ckB = b;
			_state = Idle;
			if (_ckA == _a && _ckB == _b) { ++_frames; return Result::Frame; }
			++_errors;
			return Result::Bad;
		}
		_state = Idle;
		return Result::NotMine;
	}

	uint8_t        cls() const { return _cls; }
	uint8_t        id() const { return _id; }
	uint16_t       length() const { return _len; }
	bool           truncated() const { return _len > kMaxPayload; }
	const uint8_t* payload() const { return _payload; }
	uint32_t       frames() const { return _frames; }
	uint32_t       errors() const { return _errors; }

	// For an ACK-ACK or ACK-NAK just returned as Frame: is it about
	// (cls, id)? *acked says which of the two it was.
	bool isAckFor(uint8_t cls, uint8_t id, bool* acked) const {
		if (_cls != kClassAck || _len != 2 || (_id != kIdAckAck && _id != kIdAckNak)) return false;
		if (_payload[0] != cls || _payload[1] != id) return false;
		*acked = _id == kIdAckAck;
		return true;
	}

private:
	enum State : uint8_t { Idle, Sync2, Class, Id, Len1, Len2, Payload, CkA, CkB };
	void sum(uint8_t b) { _a = (uint8_t)(_a + b); _b = (uint8_t)(_b + _a); }

	State    _state;
	uint16_t _len, _got;
	uint8_t  _cls, _id, _ckA, _ckB;
	uint8_t  _a = 0, _b = 0;
	uint8_t  _payload[kMaxPayload] = {0};
	uint32_t _frames, _errors;
};

} // namespace ubx

#endif /* UBX_PROTOCOL_H_ */
