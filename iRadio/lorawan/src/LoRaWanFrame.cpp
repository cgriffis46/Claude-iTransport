/*
 * LoRaWanFrame.cpp
 *
 *  See LoRaWanFrame.h.
 */

#include "LoRaWanFrame.h"
#include <string.h>
#include "Aes128.h"
#include "AesCmac.h"

namespace lorawan {

namespace {

void put16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
void put24(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); }
void put32(uint8_t* p, uint32_t v) { put16(p, (uint16_t)v); put16(p + 2, (uint16_t)(v >> 16)); }
uint32_t get24(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16); }
uint32_t get32(const uint8_t* p) { return get24(p) | ((uint32_t)p[3] << 24); }

// B0 (MIC) and A_i (key stream) share a layout: tag 00000000 dir DevAddr FCnt32 00 last.
void block(uint8_t b[16], uint8_t tag, uint8_t dir, uint32_t devAddr, uint32_t fCnt, uint8_t last) {
	b[0] = tag;
	b[1] = b[2] = b[3] = b[4] = 0;
	b[5] = dir;
	put32(b + 6, devAddr);
	put32(b + 10, fCnt);
	b[14] = 0;
	b[15] = last;
}

// Constant time, so a MIC check doesn't say how many bytes matched.
bool equal(const uint8_t* a, const uint8_t* b, uint8_t n) {
	uint8_t d = 0;
	for (uint8_t i = 0; i < n; ++i) d |= (uint8_t)(a[i] ^ b[i]);
	return d == 0;
}

} // namespace

void buildJoinRequest(const uint8_t appKey[16], const uint8_t joinEui[8], const uint8_t devEui[8],
                      uint16_t devNonce, uint8_t out[kJoinRequestLen]) {
	out[0] = mhdr(mtype_join_request);
	// EUIs are given most significant byte first, as printed (TTN's console
	// shows them that way); they go on the air least significant first.
	for (uint8_t i = 0; i < 8; ++i) out[1 + i] = joinEui[7 - i];
	for (uint8_t i = 0; i < 8; ++i) out[9 + i] = devEui[7 - i];
	put16(out + 17, devNonce);
	uint8_t tag[16];
	AesCmac::compute(appKey, out, 19, tag);
	memcpy(out + 19, tag, kMicLen);
}

bool parseJoinAccept(const uint8_t appKey[16], uint8_t* frame, uint8_t len, JoinAccept* ja) {
	if ((len != 17 && len != 33) || frame[0] != mhdr(mtype_join_accept)) return false;
	Aes128 aes;
	aes.setKey(appKey);
	for (uint8_t off = 1; off < len; off = (uint8_t)(off + 16)) aes.encrypt(frame + off, frame + off);
	aes.wipe();
	uint8_t tag[16];
	AesCmac::compute(appKey, frame, (size_t)(len - kMicLen), tag);
	if (!equal(tag, frame + len - kMicLen, kMicLen)) return false;
	ja->joinNonce = get24(frame + 1);
	ja->netId = get24(frame + 4);
	ja->devAddr = get32(frame + 7);
	ja->dlSettings = frame[11];
	ja->rxDelay = frame[12];
	ja->hasCfList = (len == 33);
	if (ja->hasCfList) memcpy(ja->cfList, frame + 13, 16);
	else memset(ja->cfList, 0, 16);
	return true;
}

void deriveSessionKeys(const uint8_t appKey[16], uint32_t joinNonce, uint32_t netId, uint16_t devNonce,
                       uint8_t nwkSKey[16], uint8_t appSKey[16]) {
	uint8_t b[16];
	memset(b, 0, sizeof b);
	put24(b + 1, joinNonce);
	put24(b + 4, netId);
	put16(b + 7, devNonce);
	Aes128 aes;
	aes.setKey(appKey);
	b[0] = 0x01;
	aes.encrypt(b, nwkSKey);
	b[0] = 0x02;
	aes.encrypt(b, appSKey);
	aes.wipe();
}

void cryptPayload(const uint8_t key[16], uint8_t dir, uint32_t devAddr, uint32_t fCnt,
                  uint8_t* data, uint8_t len) {
	Aes128 aes;
	aes.setKey(key);
	uint8_t a[16], s[16];
	for (uint16_t off = 0, i = 1; off < len; off = (uint16_t)(off + 16), ++i) {
		block(a, 0x01, dir, devAddr, fCnt, (uint8_t)i);
		aes.encrypt(a, s);
		const uint16_t n = (uint16_t)(len - off) < 16 ? (uint16_t)(len - off) : 16;
		for (uint16_t k = 0; k < n; ++k) data[off + k] ^= s[k];
	}
	aes.wipe();
	memset(s, 0, sizeof s);
}

void dataMic(const uint8_t nwkSKey[16], uint8_t dir, uint32_t devAddr, uint32_t fCnt,
             const uint8_t* msg, uint8_t len, uint8_t mic[kMicLen]) {
	uint8_t b0[16], tag[16];
	block(b0, 0x49, dir, devAddr, fCnt, len);
	AesCmac c(nwkSKey);
	c.start();
	c.update(b0, 16);
	c.update(msg, len);
	c.finish(tag);
	memcpy(mic, tag, kMicLen);
}

uint8_t buildUplink(const uint8_t nwkSKey[16], const uint8_t appSKey[16], const Uplink& u,
                    uint8_t* out, uint8_t cap) {
	if (u.fOptsLen > kMaxFOpts) return 0;
	if (u.hasPort && u.port == 0 && u.fOptsLen) return 0;   // MAC commands in one place only
	if (!u.hasPort && u.payloadLen) return 0;
	const uint16_t total = (uint16_t)(kDataOverhead + u.fOptsLen + (u.hasPort ? 1 + u.payloadLen : 0));
	if (total > cap || total > 255) return 0;
	uint8_t n = 0;
	out[n++] = mhdr(u.confirmed ? mtype_confirmed_up : mtype_unconfirmed_up);
	put32(out + n, u.devAddr);
	n = (uint8_t)(n + 4);
	out[n++] = (uint8_t)((u.fCtrl & (kFCtrlAdr | kFCtrlAdrAckReq | kFCtrlAck)) | u.fOptsLen);
	put16(out + n, (uint16_t)u.fCnt);
	n = (uint8_t)(n + 2);
	if (u.fOptsLen) memcpy(out + n, u.fOpts, u.fOptsLen);
	n = (uint8_t)(n + u.fOptsLen);
	if (u.hasPort) {
		out[n++] = u.port;
		if (u.payloadLen) {
			memcpy(out + n, u.payload, u.payloadLen);
			cryptPayload(u.port == 0 ? nwkSKey : appSKey, kDirUp, u.devAddr, u.fCnt, out + n, u.payloadLen);
		}
		n = (uint8_t)(n + u.payloadLen);
	}
	dataMic(nwkSKey, kDirUp, u.devAddr, u.fCnt, out, n, out + n);
	return (uint8_t)(n + kMicLen);
}

bool parseDownlinkHeader(const uint8_t* frame, uint8_t len, Downlink* d) {
	if (len < kDataOverhead) return false;
	if ((frame[0] & 0x03) != 0) return false;   // Major: LoRaWAN R1 only
	const MType t = mtypeOf(frame[0]);
	if (t != mtype_unconfirmed_down && t != mtype_confirmed_down) return false;
	d->mtype = t;
	d->devAddr = get32(frame + 1);
	d->fCtrl = frame[5];
	d->fCnt16 = (uint16_t)(frame[6] | (frame[7] << 8));
	d->fOptsOff = 8;
	d->fOptsLen = (uint8_t)(d->fCtrl & kFOptsLenMask);
	const uint8_t body = (uint8_t)(len - kMicLen);         // end of FRMPayload
	uint8_t n = (uint8_t)(d->fOptsOff + d->fOptsLen);
	if (n > body) return false;
	d->hasPort = n < body;
	d->port = 0;
	d->payloadOff = d->payloadLen = 0;
	if (d->hasPort) {
		d->port = frame[n++];
		d->payloadOff = n;
		d->payloadLen = (uint8_t)(body - n);
		if (d->port == 0 && d->fOptsLen) return false;   // MAC commands in both places
	}
	return true;
}

bool openDownlink(const uint8_t nwkSKey[16], const uint8_t appSKey[16], const Downlink& d,
                  uint32_t fCnt, uint8_t* frame, uint8_t len) {
	if (len < kDataOverhead) return false;
	uint8_t mic[kMicLen];
	dataMic(nwkSKey, kDirDown, d.devAddr, fCnt, frame, (uint8_t)(len - kMicLen), mic);
	if (!equal(mic, frame + len - kMicLen, kMicLen)) return false;
	if (d.hasPort && d.payloadLen) {
		cryptPayload(d.port == 0 ? nwkSKey : appSKey, kDirDown, d.devAddr, fCnt,
		             frame + d.payloadOff, d.payloadLen);
	}
	return true;
}

} // namespace lorawan
