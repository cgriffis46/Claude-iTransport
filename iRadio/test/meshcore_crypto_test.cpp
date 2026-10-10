// Host test for the crypto MeshCore needs: AES-128 decryption, SHA-256,
// HMAC-SHA256 and Ed25519 (Monocypher, through meshcore::LocalIdentity).
//
// Expected values: FIPS-197 C.1; FIPS 180-4's "abc" and 448 bit examples
// and the million "a"; RFC 4231 cases 1-4, 6 and 7; RFC 8032 section 7.1
// tests 1-3. The other SHA-256 lengths were computed with Python's
// hashlib, and the RFC 8032 values were also reproduced with the
// orlp/ed25519 code MeshCore uses, when this was written.
//
// g++ -std=c++17 -fno-exceptions -fno-rtti -Wall -Wextra -I../crypto/inc -I../meshcore/inc -I../third_party/monocypher meshcore_crypto_test.cpp ../meshcore/src/MeshIdentity.cpp ../third_party/monocypher/monocypher.c ../third_party/monocypher/monocypher-ed25519.c -o meshcore_crypto_test
// (compile the .c files with gcc, or g++ -x c, if your g++ refuses them)
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "Aes128.h"
#include "Sha256.h"
#include "MeshIdentity.h"
extern "C" {
#include "monocypher-ed25519.h"
}

static int g_checks = 0, g_failures = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { ++g_failures; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static std::vector<uint8_t> hx(const char* s) {
	std::vector<uint8_t> v;
	for (size_t i = 0; s[i] && s[i + 1]; i += 2) v.push_back((uint8_t)std::stoul(std::string(s + i, 2), nullptr, 16));
	return v;
}
static bool eq(const uint8_t* a, const std::vector<uint8_t>& b) { return memcmp(a, b.data(), b.size()) == 0; }

static void testAes() {
	const std::vector<uint8_t> key = hx("000102030405060708090a0b0c0d0e0f");
	const std::vector<uint8_t> pt = hx("00112233445566778899aabbccddeeff");
	const std::vector<uint8_t> ct = hx("69c4e0d86a7b0430d8cdb78070b4c55a");
	Aes128 aes(key.data());
	uint8_t out[16];
	aes.decrypt(ct.data(), out);
	CHECK(eq(out, pt));
	memcpy(out, ct.data(), 16);
	aes.decrypt(out, out);   // in place
	CHECK(eq(out, pt));
	// Decrypt undoes encrypt, for many keys and blocks.
	uint32_t x = 0x12345678u;
	auto next = [&]() { x ^= x << 13; x ^= x >> 17; x ^= x << 5; return (uint8_t)x; };
	bool all = true;
	for (int t = 0; t < 200; ++t) {
		uint8_t k[16], b[16], c[16], d[16];
		for (int i = 0; i < 16; ++i) { k[i] = next(); b[i] = next(); }
		Aes128 a(k);
		a.encrypt(b, c);
		a.decrypt(c, d);
		all &= memcmp(b, d, 16) == 0 && memcmp(b, c, 16) != 0;
	}
	CHECK(all);
}

static void testSha256() {
	uint8_t d[32];
	Sha256::hash("abc", 3, d);
	CHECK(eq(d, hx("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")));
	Sha256::hash("", 0, d);
	CHECK(eq(d, hx("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855")));
	const char* m448 = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
	Sha256::hash(m448, strlen(m448), d);
	CHECK(eq(d, hx("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1")));
	// A million "a", fed in uneven pieces.
	Sha256 s;
	std::vector<uint8_t> a(1000, 'a');
	size_t fed = 0, step = 1;
	while (fed < 1000000) {
		const size_t n = std::min(step, (size_t)1000000 - fed);
		s.update(a.data(), std::min(n, a.size()));
		fed += std::min(n, a.size());
		step = step % 997 + 7;
	}
	s.finish(d);
	CHECK(eq(d, hx("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0")));
	// Lengths around the padding boundaries (hashlib).
	struct L { size_t n; const char* h; };
	const L ls[] = {
		{55, "16fa57a0a3423a715d594516339f36189d6b5f93754a9714fef202616a9fabfe"},
		{56, "c37b44e5f1b18554b36966f4f8e08bfbf3164c4b6c10374d12d89850892073c5"},
		{63, "bbba992d2c85af960fb2987a1fd05e0aa82a3db3c740dd8982a9e273b75e36a3"},
		{64, "66bd4633ed6f71c4ecfa4763bf7ba1c8ec7612de9aa6c0578a7b675207c71e0b"},
		{65, "9f7dc47107b750a1f3d35db5d9547f24ef40da5b731b9540d4f43710a154f6c9"},
		{119, "a3ed307b730fa77c07531300c6e4a282330011d4d4caf6bb7b63ae05950f4b66"},
		{120, "8e3b15d9fea7472655aa069620b7f8c2e55ee1499f763200a7515fe826e99d20"},
	};
	for (const L& l : ls) {
		std::vector<uint8_t> m(l.n);
		for (size_t i = 0; i < l.n; ++i) m[i] = (uint8_t)(i * 7 + 1);
		Sha256::hash(m.data(), m.size(), d);
		CHECK(eq(d, hx(l.h)));
	}
}

static void testHmac() {
	struct C { std::vector<uint8_t> key, msg; const char* mac; };
	const std::string long1 = "Test Using Larger Than Block-Size Key - Hash Key First";
	const std::string long2 = "This is a test using a larger than block-size key and a larger than block-size data. "
	                          "The key needs to be hashed before being used by the HMAC algorithm.";
	std::vector<uint8_t> k4;
	for (int i = 1; i <= 25; ++i) k4.push_back((uint8_t)i);
	const std::string hi = "Hi There", jefe = "Jefe", what = "what do ya want for nothing?";
	const C cases[] = {
		{std::vector<uint8_t>(20, 0x0b), std::vector<uint8_t>(hi.begin(), hi.end()), "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"},
		{std::vector<uint8_t>(jefe.begin(), jefe.end()), std::vector<uint8_t>(what.begin(), what.end()), "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"},
		{std::vector<uint8_t>(20, 0xaa), std::vector<uint8_t>(50, 0xdd), "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe"},
		{k4, std::vector<uint8_t>(50, 0xcd), "82558a389a443c0ea4cc819899f2083a85f0faa3e578f8077a2e3ff46729665b"},
		{std::vector<uint8_t>(131, 0xaa), std::vector<uint8_t>(long1.begin(), long1.end()), "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54"},
		{std::vector<uint8_t>(131, 0xaa), std::vector<uint8_t>(long2.begin(), long2.end()), "9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2"},
	};
	for (const C& c : cases) {
		uint8_t mac[32];
		HmacSha256::compute(c.key.data(), c.key.size(), c.msg.data(), c.msg.size(), mac);
		CHECK(eq(mac, hx(c.mac)));
	}
	// In pieces, and start() reuses the key.
	HmacSha256 h(cases[1].key.data(), cases[1].key.size());
	h.update(cases[1].msg.data(), 10);
	h.update(cases[1].msg.data() + 10, cases[1].msg.size() - 10);
	uint8_t mac[32];
	h.finish(mac);
	CHECK(eq(mac, hx(cases[1].mac)));
	h.start();
	h.update(cases[1].msg.data(), cases[1].msg.size());
	h.finish(mac);
	CHECK(eq(mac, hx(cases[1].mac)));
}

static void testEd25519() {
	struct T { const char* seed; std::vector<uint8_t> msg; const char* pub; const char* sig; };
	const T ts[] = {
		{"9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60", {},
		 "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a",
		 "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"},
		{"4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb", {0x72},
		 "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c",
		 "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00"},
		{"c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7", {0xaf, 0x82},
		 "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025",
		 "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a"},
	};
	for (const T& t : ts) {
		meshcore::LocalIdentity id;
		CHECK(id.fromSeed(hx(t.seed).data()) && id.valid());
		CHECK(eq(id.pubKey(), hx(t.pub)));
		uint8_t sig[64];
		id.sign(sig, t.msg.data(), t.msg.size());
		CHECK(eq(sig, hx(t.sig)));
		CHECK(meshcore::LocalIdentity::verify(id.pubKey(), sig, t.msg.data(), t.msg.size()));
		sig[17] ^= 0x04;
		CHECK(!meshcore::LocalIdentity::verify(id.pubKey(), sig, t.msg.data(), t.msg.size()));
		sig[17] ^= 0x04;
		std::vector<uint8_t> other = t.msg;
		other.push_back(0);
		CHECK(!meshcore::LocalIdentity::verify(id.pubKey(), sig, other.data(), other.size()));
	}
	// Public keys starting 00 or FF are refused, as MeshCore refuses them.
	int refused = 0, tried = 0;
	for (uint32_t i = 0; i < 4000 && refused < 2; ++i) {
		uint8_t seed[32] = {0}, s2[32], sk[64], pub[32];
		memcpy(seed, &i, sizeof i);
		memcpy(s2, seed, 32);
		crypto_ed25519_key_pair(sk, pub, s2);
		if (pub[0] != 0x00 && pub[0] != 0xFF) continue;
		++tried;
		meshcore::LocalIdentity id;
		if (!id.fromSeed(seed) && !id.valid() && id.pubKey()[0] == 0 && id.pubKey()[1] == 0) ++refused;
	}
	CHECK(tried >= 2 && refused == tried);
}

int main() {
	testAes();
	testSha256();
	testHmac();
	testEd25519();
	std::printf("meshcore_crypto_test: %d checks, %d failures\n", g_checks, g_failures);
	return g_failures ? 1 : 0;
}
