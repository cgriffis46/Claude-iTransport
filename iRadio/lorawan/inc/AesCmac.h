#pragma once
// AES-CMAC (RFC 4493, NIST SP 800-38B) over Aes128, fed in pieces:
// LoRaWAN's MIC is the first four bytes of CMAC(key, B0 | message), with
// B0 built separately from the message. Header-only, no heap. Checked
// against RFC 4493's four examples (iRadio/test/aes_cmac_test.cpp).
//
//     AesCmac mac(key);
//     mac.update(b0, 16);
//     mac.update(msg, len);
//     uint8_t tag[16];
//     mac.finish(tag);

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "Aes128.h"

class AesCmac {
public:
	explicit AesCmac(const uint8_t key[16]) : _aes(key) { start(); }
	~AesCmac() {
		volatile uint8_t* p = _x;
		for (int i = 0; i < 16; ++i) p[i] = 0;
	}

	// Begins a new message with the same key.
	void start() {
		memset(_x, 0, sizeof _x);
		memset(_buf, 0, sizeof _buf);
		_n = 0;
	}

	void update(const uint8_t* data, size_t len) {
		for (size_t i = 0; i < len; ++i) {
			if (_n == 16) {   // a full block, and more is coming: it is not the last
				for (int k = 0; k < 16; ++k) _x[k] ^= _buf[k];
				_aes.encrypt(_x, _x);
				_n = 0;
			}
			_buf[_n++] = data[i];
		}
	}

	// The 16 byte tag. start() again before the next message.
	void finish(uint8_t tag[16]) {
		uint8_t k1[16], k2[16];
		subkeys(k1, k2);
		uint8_t last[16];
		if (_n == 16) {
			for (int k = 0; k < 16; ++k) last[k] = (uint8_t)(_buf[k] ^ k1[k]);
		} else {   // empty or partial: pad with 10..0, use K2
			for (int k = 0; k < 16; ++k) {
				const uint8_t b = k < _n ? _buf[k] : (k == _n ? 0x80 : 0x00);
				last[k] = (uint8_t)(b ^ k2[k]);
			}
		}
		for (int k = 0; k < 16; ++k) _x[k] ^= last[k];
		_aes.encrypt(_x, tag);
	}

	// One call for a whole message.
	static void compute(const uint8_t key[16], const uint8_t* data, size_t len, uint8_t tag[16]) {
		AesCmac m(key);
		m.update(data, len);
		m.finish(tag);
	}

private:
	// K1 = L << 1 (^ Rb), K2 = K1 << 1 (^ Rb), L = AES(key, 0), Rb = 0x87.
	void subkeys(uint8_t k1[16], uint8_t k2[16]) const {
		uint8_t l[16] = {0};
		_aes.encrypt(l, l);
		shift(l, k1);
		shift(k1, k2);
	}
	static void shift(const uint8_t in[16], uint8_t out[16]) {
		const uint8_t carry = in[0] & 0x80;
		for (int i = 0; i < 15; ++i) out[i] = (uint8_t)((in[i] << 1) | (in[i + 1] >> 7));
		out[15] = (uint8_t)(in[15] << 1);
		if (carry) out[15] ^= 0x87;
	}

	Aes128  _aes;
	uint8_t _x[16];     // the running CBC value
	uint8_t _buf[16];   // the block not yet processed (it may be the last)
	uint8_t _n;
};
