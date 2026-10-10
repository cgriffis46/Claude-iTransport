/*
 * MeshCrypto.cpp
 *
 *  See MeshCrypto.h.
 */

#include "MeshCrypto.h"
#include <string.h>
#include "Aes128.h"
#include "Sha256.h"

namespace meshcore {

const uint8_t kPublicChannelKey[16] = {0x8b, 0x33, 0x87, 0xe9, 0xc5, 0xcd, 0xea, 0x6a,
                                       0xc9, 0xe5, 0xed, 0xba, 0xa1, 0x15, 0xcd, 0x72};

bool Channel::set(const uint8_t* key, uint8_t len) {
	if (len != 16 && len != 32) return false;
	memset(secret, 0, sizeof secret);
	memcpy(secret, key, len);
	uint8_t d[Sha256::kDigest];
	Sha256::hash(key, len, d);
	hash = d[0];
	return true;
}

uint8_t encryptThenMac(const uint8_t secret[kSecretSize], const uint8_t* plain, uint8_t len, uint8_t* out, uint8_t cap) {
	const uint16_t enc = (uint16_t)((len + 15u) / 16u * 16u);
	if (len == 0 || kMacSize + enc > cap) return 0;
	Aes128 aes(secret);
	uint8_t* c = out + kMacSize;
	for (uint16_t off = 0; off < enc; off = (uint16_t)(off + 16)) {
		uint8_t block[16];
		memset(block, 0, sizeof block);
		const uint16_t n = (uint16_t)(len - off) < 16 ? (uint16_t)(len - off) : 16;
		memcpy(block, plain + off, n);
		aes.encrypt(block, c + off);
	}
	uint8_t mac[HmacSha256::kMac];
	HmacSha256::compute(secret, kSecretSize, c, enc, mac);
	memcpy(out, mac, kMacSize);
	return (uint8_t)(kMacSize + enc);
}

uint8_t macThenDecrypt(const uint8_t secret[kSecretSize], const uint8_t* in, uint8_t len, uint8_t* out) {
	if (len <= kMacSize || (len - kMacSize) % 16 != 0) return 0;
	const uint8_t enc = (uint8_t)(len - kMacSize);
	uint8_t mac[HmacSha256::kMac];
	HmacSha256::compute(secret, kSecretSize, in + kMacSize, enc, mac);
	if ((uint8_t)((mac[0] ^ in[0]) | (mac[1] ^ in[1])) != 0) return 0;
	Aes128 aes(secret);
	for (uint8_t off = 0; off < enc; off = (uint8_t)(off + 16)) aes.decrypt(in + kMacSize + off, out + off);
	return enc;
}

} // namespace meshcore
