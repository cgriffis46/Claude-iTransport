#pragma once
// SHA-256 (FIPS 180-4) and HMAC-SHA256 (RFC 2104), header-only, no heap.
// MeshCore uses them for channel hashes, packet hashes (duplicate
// suppression) and its 2-byte encrypt-then-MAC tags. Checked against the
// FIPS 180-4 / NIST examples and RFC 4231's HMAC vectors
// (iRadio/test/meshcore_crypto_test.cpp), and against Python's hashlib
// and hmac on random inputs when written.

#include <stddef.h>
#include <stdint.h>
#include <string.h>

class Sha256 {
public:
	static const uint8_t kDigest = 32, kBlock = 64;

	Sha256() { start(); }
	~Sha256() { wipe(); }

	void start() {
		static const uint32_t h0[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
		                               0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
		memcpy(_h, h0, sizeof _h);
		_n = 0;
		_bits = 0;
	}

	void update(const void* data, size_t len) {
		const uint8_t* p = (const uint8_t*)data;
		_bits += (uint64_t)len * 8u;
		while (len) {
			const size_t take = (size_t)(kBlock - _n) < len ? (size_t)(kBlock - _n) : len;
			memcpy(_buf + _n, p, take);
			_n = (uint8_t)(_n + take);
			p += take;
			len -= take;
			if (_n == kBlock) { block(_buf); _n = 0; }
		}
	}

	void finish(uint8_t digest[kDigest]) {
		const uint64_t bits = _bits;
		uint8_t pad = 0x80;
		update(&pad, 1);
		pad = 0;
		while (_n != 56) update(&pad, 1);
		uint8_t len[8];
		for (int i = 0; i < 8; ++i) len[i] = (uint8_t)(bits >> (56 - 8 * i));
		update(len, 8);
		for (int i = 0; i < 8; ++i) {
			digest[4 * i]     = (uint8_t)(_h[i] >> 24);
			digest[4 * i + 1] = (uint8_t)(_h[i] >> 16);
			digest[4 * i + 2] = (uint8_t)(_h[i] >> 8);
			digest[4 * i + 3] = (uint8_t)_h[i];
		}
		wipe();
	}

	static void hash(const void* data, size_t len, uint8_t digest[kDigest]) {
		Sha256 s;
		s.update(data, len);
		s.finish(digest);
	}

	void wipe() {
		volatile uint8_t* p = _buf;
		for (size_t i = 0; i < sizeof _buf; ++i) p[i] = 0;
		volatile uint32_t* h = _h;
		for (int i = 0; i < 8; ++i) h[i] = 0;
	}

private:
	static uint32_t ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

	void block(const uint8_t* p) {
		static const uint32_t k[64] = {
			0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
			0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
			0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
			0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
			0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
			0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
			0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
			0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};
		uint32_t w[64];
		for (int i = 0; i < 16; ++i)
			w[i] = ((uint32_t)p[4 * i] << 24) | ((uint32_t)p[4 * i + 1] << 16) | ((uint32_t)p[4 * i + 2] << 8) | p[4 * i + 3];
		for (int i = 16; i < 64; ++i) {
			const uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
			const uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
			w[i] = w[i - 16] + s0 + w[i - 7] + s1;
		}
		uint32_t a = _h[0], b = _h[1], c = _h[2], d = _h[3], e = _h[4], f = _h[5], g = _h[6], h = _h[7];
		for (int i = 0; i < 64; ++i) {
			const uint32_t t1 = h + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
			const uint32_t t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
			h = g; g = f; f = e; e = d + t1;
			d = c; c = b; b = a; a = t1 + t2;
		}
		_h[0] += a; _h[1] += b; _h[2] += c; _h[3] += d;
		_h[4] += e; _h[5] += f; _h[6] += g; _h[7] += h;
	}

	uint32_t _h[8];
	uint8_t  _buf[kBlock];
	uint8_t  _n;
	uint64_t _bits;
};

class HmacSha256 {
public:
	static const uint8_t kMac = Sha256::kDigest;

	HmacSha256(const uint8_t* key, size_t keyLen) {
		memset(_k, 0, sizeof _k);
		if (keyLen > Sha256::kBlock) Sha256::hash(key, keyLen, _k);   // long keys are hashed first
		else memcpy(_k, key, keyLen);
		start();
	}
	~HmacSha256() {
		volatile uint8_t* p = _k;
		for (size_t i = 0; i < sizeof _k; ++i) p[i] = 0;
	}

	void start() {
		uint8_t ipad[Sha256::kBlock];
		for (int i = 0; i < Sha256::kBlock; ++i) ipad[i] = (uint8_t)(_k[i] ^ 0x36);
		_inner.start();
		_inner.update(ipad, sizeof ipad);
		memset(ipad, 0, sizeof ipad);
	}
	void update(const void* data, size_t len) { _inner.update(data, len); }

	// The full 32 byte MAC; callers that send a short tag take its first bytes.
	void finish(uint8_t mac[kMac]) {
		uint8_t inner[Sha256::kDigest], opad[Sha256::kBlock];
		_inner.finish(inner);
		for (int i = 0; i < Sha256::kBlock; ++i) opad[i] = (uint8_t)(_k[i] ^ 0x5c);
		Sha256 outer;
		outer.update(opad, sizeof opad);
		outer.update(inner, sizeof inner);
		outer.finish(mac);
		memset(inner, 0, sizeof inner);
		memset(opad, 0, sizeof opad);
	}

	static void compute(const uint8_t* key, size_t keyLen, const void* data, size_t len, uint8_t mac[kMac]) {
		HmacSha256 h(key, keyLen);
		h.update(data, len);
		h.finish(mac);
	}

private:
	uint8_t _k[Sha256::kBlock];
	Sha256  _inner;
};
