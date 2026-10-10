/*
 * MeshIdentity.cpp
 *
 *  See MeshIdentity.h.
 */

#include "MeshIdentity.h"
#include <string.h>
extern "C" {
#include "monocypher.h"
#include "monocypher-ed25519.h"
}

namespace meshcore {

bool LocalIdentity::fromSeed(const uint8_t seed[kSeedSize]) {
	uint8_t s[kSeedSize];
	memcpy(s, seed, sizeof s);   // Monocypher wipes the seed it is given
	crypto_ed25519_key_pair(_secret, _pub, s);
	_valid = _pub[0] != 0x00 && _pub[0] != 0xFF;
	if (!_valid) clear();
	return _valid;
}

void LocalIdentity::sign(uint8_t sig[kSignatureSize], const uint8_t* msg, size_t len) const {
	crypto_ed25519_sign(sig, _secret, msg, len);
}

bool LocalIdentity::verify(const uint8_t pub[kPubKeySize], const uint8_t sig[kSignatureSize], const uint8_t* msg, size_t len) {
	return crypto_ed25519_check(sig, pub, msg, len) == 0;
}

void LocalIdentity::clear() {
	crypto_wipe(_secret, sizeof _secret);
	memset(_pub, 0, sizeof _pub);
	_valid = false;
}

} // namespace meshcore
