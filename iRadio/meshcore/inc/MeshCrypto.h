/*
 * MeshCrypto.h
 *
 *  MeshCore's symmetric layer (payload version 1), as in its
 *  src/Utils.cpp:
 *
 *    encrypt-then-MAC   AES-128 in ECB mode with the first 16 bytes of
 *                       the 32 byte secret, the last block zero padded;
 *                       then HMAC-SHA256 keyed with all 32 bytes of the
 *                       secret over the ciphertext, the first 2 bytes
 *                       put in front:  MAC 2 | ciphertext (16 x n)
 *    MAC-then-decrypt   the reverse; the plaintext comes back padded to
 *                       a multiple of 16 with zeros, the caller knows
 *                       (or finds) the real length
 *
 *  A group channel is a 16 or 32 byte key (a "PSK", given as base64 in
 *  MeshCore's apps), zero padded to 32 bytes; its hash, sent in front of
 *  each group packet, is the first byte of SHA-256 over the key as given
 *  (16 or 32 bytes). The well-known "Public" channel's key is
 *  8b3387e9c5cdea6ac9e5edbaa115cd72 (base64 izOH6cXN6mrJ5e26oRXNcg==).
 *
 *  ECB and a 2 byte tag are MeshCore's choices, not ours: a 16 bit MAC
 *  stops accidents and casual forgery, not a determined attacker (1 in
 *  65536 guesses passes). Checked against MeshCore's own Utils.cpp
 *  (iRadio/test/meshcore_packet_test.cpp).
 */

#ifndef MESHCRYPTO_H_
#define MESHCRYPTO_H_

#include <stdint.h>

namespace meshcore {

static const uint8_t kMacSize = 2;          // CIPHER_MAC_SIZE
static const uint8_t kSecretSize = 32;      // PUB_KEY_SIZE: secrets are kept as 32 bytes

struct Channel {
	uint8_t secret[kSecretSize];   // the key, zero padded
	uint8_t hash;                  // first byte of SHA-256 of the key

	// key: 16 or 32 bytes. False for any other length.
	bool set(const uint8_t* key, uint8_t len);
};

extern const uint8_t kPublicChannelKey[16];

// Encrypts len bytes of plain into out as MAC | ciphertext. Returns the
// bytes written (2 + len rounded up to 16), or 0 if that would pass cap.
uint8_t encryptThenMac(const uint8_t secret[kSecretSize], const uint8_t* plain, uint8_t len, uint8_t* out, uint8_t cap);

// Checks the MAC of in (MAC | ciphertext) and decrypts into out. Returns
// the plaintext length (a multiple of 16, zero padded), or 0 if the MAC
// doesn't match or the length is wrong. out needs len - 2 bytes.
uint8_t macThenDecrypt(const uint8_t secret[kSecretSize], const uint8_t* in, uint8_t len, uint8_t* out);

} // namespace meshcore

#endif /* MESHCRYPTO_H_ */
