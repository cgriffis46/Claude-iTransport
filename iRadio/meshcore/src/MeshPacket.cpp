/*
 * MeshPacket.cpp
 *
 *  See MeshPacket.h.
 */

#include "MeshPacket.h"
#include <string.h>
#include "Sha256.h"

namespace meshcore {

bool Packet::validPathLen(uint8_t pathLen) {
	const uint8_t size = (uint8_t)((pathLen >> 6) + 1);
	if (size == 4) return false;   // reserved
	return (uint16_t)(pathLen & 63) * size <= kMaxPath;
}

uint8_t Packet::encode(uint8_t* out, uint8_t cap) const {
	if (!validPathLen(pathLen) || payloadLen == 0 || payloadLen > kMaxPayload) return 0;
	const uint16_t total = (uint16_t)(1 + (hasTransportCodes() ? 4 : 0) + 1 + pathBytes() + payloadLen);
	if (total > cap || total > kMaxPacket) return 0;
	uint8_t n = 0;
	out[n++] = header;
	if (hasTransportCodes()) {
		for (uint8_t i = 0; i < 2; ++i) {
			out[n++] = (uint8_t)transport[i];
			out[n++] = (uint8_t)(transport[i] >> 8);
		}
	}
	out[n++] = pathLen;
	memcpy(out + n, path, pathBytes());
	n = (uint8_t)(n + pathBytes());
	memcpy(out + n, payload, payloadLen);
	return (uint8_t)(n + payloadLen);
}

bool Packet::decode(const uint8_t* in, uint8_t len) {
	uint8_t n = 0;
	if (len < 3) return false;
	header = in[n++];
	if (hasTransportCodes()) {
		if (len < 7) return false;
		for (uint8_t i = 0; i < 2; ++i) {
			transport[i] = (uint16_t)(in[n] | (in[n + 1] << 8));
			n = (uint8_t)(n + 2);
		}
	} else {
		transport[0] = transport[1] = 0;
	}
	pathLen = in[n++];
	if (!validPathLen(pathLen)) return false;
	if ((uint16_t)n + pathBytes() >= len) return false;   // no payload
	memcpy(path, in + n, pathBytes());
	n = (uint8_t)(n + pathBytes());
	if (len - n > kMaxPayload) return false;
	payloadLen = (uint8_t)(len - n);
	memcpy(payload, in + n, payloadLen);
	return true;
}

void Packet::hash(uint8_t out[kHashSize]) const {
	Sha256 sha;
	const uint8_t t = payloadType();
	sha.update(&t, 1);
	if (t == payload_trace) {
		const uint8_t pl[2] = {pathLen, 0};   // MeshCore hashes its uint16_t path_len, little-endian
		sha.update(pl, 2);
	}
	sha.update(payload, payloadLen);
	uint8_t d[Sha256::kDigest];
	sha.finish(d);
	memcpy(out, d, kHashSize);
}

} // namespace meshcore
