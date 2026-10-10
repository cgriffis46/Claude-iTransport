// Host test for Aes128 and AesCmac against the published vectors:
// FIPS-197 Appendix C.1 (AES-128) and RFC 4493 section 4 (AES-CMAC).
//
// g++ -std=gnu++14 -fno-exceptions -fno-rtti -Wall -Wextra -I../lorawan/inc aes_cmac_test.cpp -o aes_cmac_test
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "AesCmac.h"

static int g_checks = 0, g_failures = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { ++g_failures; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static std::vector<uint8_t> hex(const char* s) {
	std::vector<uint8_t> v;
	std::string t;
	for (const char* p = s; *p; ++p) if (*p != ' ') t += *p;
	for (size_t i = 0; i + 1 < t.size(); i += 2) v.push_back((uint8_t)std::stoul(t.substr(i, 2), nullptr, 16));
	return v;
}

static void testFips197() {
	// Appendix C.1: AES-128.
	const std::vector<uint8_t> key = hex("000102030405060708090a0b0c0d0e0f");
	const std::vector<uint8_t> pt  = hex("00112233445566778899aabbccddeeff");
	const std::vector<uint8_t> ct  = hex("69c4e0d86a7b0430d8cdb78070b4c55a");
	Aes128 aes(key.data());
	uint8_t out[16];
	aes.encrypt(pt.data(), out);
	CHECK(std::memcmp(out, ct.data(), 16) == 0);
	// In place.
	uint8_t buf[16];
	std::memcpy(buf, pt.data(), 16);
	aes.encrypt(buf, buf);
	CHECK(std::memcmp(buf, ct.data(), 16) == 0);
	// Appendix B (the cipher example): key 2b7e1516..., input 3243f6a8...
	const std::vector<uint8_t> k2 = hex("2b7e151628aed2a6abf7158809cf4f3c");
	const std::vector<uint8_t> p2 = hex("3243f6a8885a308d313198a2e0370734");
	const std::vector<uint8_t> c2 = hex("3925841d02dc09fbdc118597196a0b32");
	Aes128 aes2(k2.data());
	aes2.encrypt(p2.data(), out);
	CHECK(std::memcmp(out, c2.data(), 16) == 0);
}

static void testRfc4493() {
	const std::vector<uint8_t> key = hex("2b7e151628aed2a6abf7158809cf4f3c");
	const std::vector<uint8_t> msg = hex(
		"6bc1bee22e409f96e93d7e117393172a"
		"ae2d8a571e03ac9c9eb76fac45af8e51"
		"30c81c46a35ce411e5fbc1191a0a52ef"
		"f69f2445df4f9b17ad2b417be66c3710");
	struct Case { size_t len; const char* tag; };
	const Case cases[] = {
		{0,  "bb1d6929e95937287fa37d129b756746"},
		{16, "070a16b46b4d4144f79bdd9dd04a287c"},
		{40, "dfa66747de9ae63030ca32611497c827"},
		{64, "51f0bebf7e3b9d92fc49741779363cfe"},
	};
	for (const Case& c : cases) {
		uint8_t tag[16];
		AesCmac::compute(key.data(), msg.data(), c.len, tag);
		CHECK(std::memcmp(tag, hex(c.tag).data(), 16) == 0);
		// The same, fed one byte at a time and in odd pieces.
		AesCmac m(key.data());
		for (size_t i = 0; i < c.len; ++i) m.update(&msg[i], 1);
		uint8_t t1[16];
		m.finish(t1);
		CHECK(std::memcmp(t1, tag, 16) == 0);
		m.start();
		size_t done = 0;
		const size_t pieces[] = {3, 13, 1, 16, 7, 24};
		for (size_t p : pieces) {
			const size_t n = done + p > c.len ? c.len - done : p;
			m.update(msg.data() + done, n);
			done += n;
		}
		m.update(msg.data() + done, c.len - done);
		uint8_t t2[16];
		m.finish(t2);
		CHECK(std::memcmp(t2, tag, 16) == 0);
	}
}

int main() {
	testFips197();
	testRfc4493();
	std::printf("aes_cmac_test: %d checks, %d failures\n", g_checks, g_failures);
	return g_failures ? 1 : 0;
}
