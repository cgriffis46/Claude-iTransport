/*
 * MeshMessages.cpp
 *
 *  See MeshMessages.h.
 */

#include "MeshMessages.h"
#include <string.h>

namespace meshcore {

namespace {

void put16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
void put32(uint8_t* p, uint32_t v) { put16(p, (uint16_t)v); put16(p + 2, (uint16_t)(v >> 16)); }
uint16_t get16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t get32(const uint8_t* p) { return get16(p) | ((uint32_t)get16(p + 2) << 16); }

} // namespace

bool buildGroupPacket(const Channel& ch, PayloadType t, const uint8_t* plain, uint8_t len, Packet& pkt) {
	if (t != payload_grp_txt && t != payload_grp_data) return false;
	pkt.begin(t, route_flood);
	pkt.payload[0] = ch.hash;
	const uint8_t n = encryptThenMac(ch.secret, plain, len, pkt.payload + 1, (uint8_t)(kMaxPayload - 1));
	if (n == 0) return false;
	pkt.payloadLen = (uint8_t)(1 + n);
	return true;
}

uint8_t utf8Prefix(const char* text, uint8_t max) {
	uint8_t off = 0;
	while (text[off] != 0) {
		const uint8_t c = (uint8_t)text[off];
		uint8_t n;
		if (c <= 0x7F) n = 1;
		else if (c >= 0xC2 && c <= 0xDF) n = 2;
		else if (c >= 0xE0 && c <= 0xEF) n = 3;
		else if (c >= 0xF0 && c <= 0xF4) n = 4;
		else break;
		if (off + n > max) break;
		bool whole = true;
		for (uint8_t i = 1; i < n; ++i) {
			const uint8_t k = (uint8_t)text[off + i];
			if (k == 0 || (k & 0xC0) != 0x80) { whole = false; break; }
		}
		if (!whole) break;
		off = (uint8_t)(off + n);
	}
	return off;
}

uint8_t AdvertData::encode(uint8_t* out) const {
	uint8_t n = 1;
	out[0] = (uint8_t)(type & 0x0F);
	if (hasLocation) {
		out[0] |= 0x10;
		put32(out + n, (uint32_t)latE6); n = (uint8_t)(n + 4);
		put32(out + n, (uint32_t)lonE6); n = (uint8_t)(n + 4);
	}
	if (feature1) { out[0] |= 0x20; put16(out + n, feature1); n = (uint8_t)(n + 2); }
	if (feature2) { out[0] |= 0x40; put16(out + n, feature2); n = (uint8_t)(n + 2); }
	const uint8_t nl = utf8Prefix(name, (uint8_t)(kMaxAdvertData - n));
	if (nl) {
		out[0] |= 0x80;
		memcpy(out + n, name, nl);
		n = (uint8_t)(n + nl);
	}
	return n;
}

bool AdvertData::decode(const uint8_t* in, uint8_t len) {
	if (len < 1) return false;
	const uint8_t f = in[0];
	uint8_t n = 1;
	type = (uint8_t)(f & 0x0F);
	hasLocation = (f & 0x10) != 0;
	latE6 = lonE6 = 0;
	feature1 = feature2 = 0;
	name[0] = 0;
	if (hasLocation) {
		if (n + 8 > len) return false;
		latE6 = (int32_t)get32(in + n); lonE6 = (int32_t)get32(in + n + 4);
		n = (uint8_t)(n + 8);
	}
	if (f & 0x20) { if (n + 2 > len) return false; feature1 = get16(in + n); n = (uint8_t)(n + 2); }
	if (f & 0x40) { if (n + 2 > len) return false; feature2 = get16(in + n); n = (uint8_t)(n + 2); }
	if (f & 0x80) {
		uint8_t nl = (uint8_t)(len - n);
		if (nl > kMaxAdvertData) nl = kMaxAdvertData;   // name holds 32 (parseAdvert never passes more)
		memcpy(name, in + n, nl);
		name[nl] = 0;
	}
	return true;
}

bool buildAdvert(const LocalIdentity& id, uint32_t timestamp, const AdvertData& data, bool flood, Packet& pkt) {
	uint8_t app[kMaxAdvertData];
	const uint8_t appLen = data.encode(app);
	return buildAdvertRaw(id, timestamp, app, appLen, flood, pkt);
}

bool buildAdvertRaw(const LocalIdentity& id, uint32_t timestamp, const uint8_t* app, uint8_t appLen, bool flood, Packet& pkt) {
	if (!id.valid() || appLen > kMaxAdvertData) return false;
	pkt.begin(payload_advert, flood ? route_flood : route_direct);
	uint8_t* p = pkt.payload;
	memcpy(p, id.pubKey(), kPubKeySize);
	put32(p + kPubKeySize, timestamp);
	memcpy(p + kPubKeySize + 4 + kSignatureSize, app, appLen);
	// Signed: public key | timestamp | app data.
	uint8_t msg[kPubKeySize + 4 + kMaxAdvertData];
	memcpy(msg, p, kPubKeySize + 4);
	memcpy(msg + kPubKeySize + 4, app, appLen);
	id.sign(p + kPubKeySize + 4, msg, (size_t)(kPubKeySize + 4 + appLen));
	pkt.payloadLen = (uint8_t)(kPubKeySize + 4 + kSignatureSize + appLen);
	return true;
}

bool parseAdvert(const Packet& pkt, Advert* a) {
	if (pkt.payloadType() != payload_advert || pkt.payloadLen < kPubKeySize + 4 + kSignatureSize) return false;
	const uint8_t* p = pkt.payload;
	memcpy(a->pubKey, p, kPubKeySize);
	a->timestamp = get32(p + kPubKeySize);
	const uint8_t* sig = p + kPubKeySize + 4;
	uint8_t appLen = (uint8_t)(pkt.payloadLen - (kPubKeySize + 4 + kSignatureSize));
	if (appLen > kMaxAdvertData) appLen = kMaxAdvertData;   // as MeshCore: the rest isn't signed or read
	a->appLen = appLen;
	memcpy(a->appData, sig + kSignatureSize, appLen);
	uint8_t msg[kPubKeySize + 4 + kMaxAdvertData];
	memcpy(msg, p, kPubKeySize + 4);
	memcpy(msg + kPubKeySize + 4, a->appData, appLen);
	if (!LocalIdentity::verify(a->pubKey, sig, msg, (size_t)(kPubKeySize + 4 + appLen))) return false;
	a->dataValid = appLen > 0 && a->data.decode(a->appData, appLen);
	return true;
}

bool buildGroupText(const Channel& ch, uint32_t timestamp, const char* sender, const char* text, Packet& pkt) {
	uint8_t plain[5 + kMaxTextLen];
	put32(plain, timestamp);
	plain[4] = 0;   // TXT_TYPE_PLAIN, attempt 0
	uint8_t n = 5;
	const size_t sl = strlen(sender);
	if (sl + 2 > kMaxTextLen) return false;
	memcpy(plain + n, sender, sl);
	n = (uint8_t)(n + sl);
	plain[n++] = ':';
	plain[n++] = ' ';
	// The message cut to fit, at a whole character (MeshCore cuts at a byte).
	const uint8_t tl = utf8Prefix(text, (uint8_t)(kMaxTextLen - (n - 5)));
	memcpy(plain + n, text, tl);
	n = (uint8_t)(n + tl);
	return buildGroupPacket(ch, payload_grp_txt, plain, n, pkt);
}

bool buildGroupData(const Channel& ch, uint16_t dataType, const uint8_t* data, uint8_t len, Packet& pkt) {
	if (len > kMaxGroupData) return false;
	uint8_t plain[3 + kMaxGroupData];
	put16(plain, dataType);
	plain[2] = len;
	if (len) memcpy(plain + 3, data, len);
	return buildGroupPacket(ch, payload_grp_data, plain, (uint8_t)(3 + len), pkt);
}

bool openGroup(const Packet& pkt, const Channel* channels, uint8_t count, GroupMessage* m) {
	const PayloadType t = pkt.payloadType();
	if ((t != payload_grp_txt && t != payload_grp_data) || pkt.payloadLen <= 1 + kMacSize) return false;
	for (uint8_t i = 0; i < count; ++i) {
		if (channels[i].hash != pkt.payload[0]) continue;
		uint8_t plain[kMaxPayload];
		const uint8_t n = macThenDecrypt(channels[i].secret, pkt.payload + 1, (uint8_t)(pkt.payloadLen - 1), plain);
		if (n == 0) continue;   // another channel with the same hash byte, or a bad MAC
		m->channel = i;
		m->isText = t == payload_grp_txt;
		if (m->isText) {
			if (n < 5 || (plain[4] >> 2) != 0) return false;   // only plain text
			m->timestamp = get32(plain);
			m->textFlags = plain[4];
			uint8_t l = 0;
			while (5 + l < n && plain[5 + l] != 0) ++l;   // the padding is zeros
			memcpy(m->data, plain + 5, l);
			m->data[l] = 0;
			m->len = l;
		} else {
			if (n < 3 || plain[2] > n - 3) return false;
			m->dataType = get16(plain);
			m->len = plain[2];
			memcpy(m->data, plain + 3, m->len);
			m->timestamp = 0;
		}
		return true;
	}
	return false;
}

} // namespace meshcore
