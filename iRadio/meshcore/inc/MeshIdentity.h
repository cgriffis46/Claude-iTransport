/*
 * MeshIdentity.h
 *
 *  A MeshCore node's identity: an Ed25519 key pair (RFC 8032). The public
 *  key is the node's address (its first byte is the "node hash" in paths
 *  and messages); adverts are signed with the private key. Ed25519 is
 *  Monocypher's (iRadio/third_party/monocypher), which gives the same
 *  public keys and signatures for a seed as the orlp/ed25519 code MeshCore
 *  uses (checked in meshcore_crypto_test).
 *
 *  Kept here: the 64 byte secret key (seed and public key, Monocypher's
 *  form) and the public key. MeshCore's firmware stores the expanded
 *  orlp form instead (SHA-512 of the seed); an identity exported from a
 *  MeshCore device can't be loaded here, and none needs to be: a node
 *  made here makes its own from a 32 byte random seed and keeps that.
 *  MeshCore refuses public keys starting 00 or FF, so fromSeed() does too.
 */

#ifndef MESHIDENTITY_H_
#define MESHIDENTITY_H_

#include <stdint.h>
#include <stddef.h>

namespace meshcore {

static const uint8_t kPubKeySize = 32, kSeedSize = 32, kSignatureSize = 64;

class LocalIdentity {
public:
	LocalIdentity() { clear(); }
	~LocalIdentity() { clear(); }

	// From a 32 byte random seed (keep the seed: it is the identity).
	// False if the public key would start 00 or FF (pick another seed).
	bool fromSeed(const uint8_t seed[kSeedSize]);
	bool valid() const { return _valid; }

	const uint8_t* pubKey() const { return _pub; }
	uint8_t nodeHash() const { return _pub[0]; }

	void sign(uint8_t sig[kSignatureSize], const uint8_t* msg, size_t len) const;
	static bool verify(const uint8_t pub[kPubKeySize], const uint8_t sig[kSignatureSize], const uint8_t* msg, size_t len);

	void clear();

private:
	uint8_t _secret[64];
	uint8_t _pub[kPubKeySize];
	bool    _valid;
};

} // namespace meshcore

#endif /* MESHIDENTITY_H_ */
