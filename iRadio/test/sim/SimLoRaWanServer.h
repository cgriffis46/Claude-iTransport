/*
 * SimLoRaWanServer.h
 *
 * For lorawan_mac_test: a gateway and a LoRaWAN 1.0.4 network server on
 * sim::Air, playing The Things Network's part for one device on US915
 * sub-band 2 (channels 8-15 and 65).
 *
 * The gateway hears any uplink on those channels at any data rate and
 * sends each downlink from its own simulated SX1276 at the exact time:
 * the uplink's end plus RECEIVE_DELAY1 (or 5 s for a join accept) for
 * RX1, a second more for RX2. The server checks the join request's MIC
 * and that DevNonce counts up (as TTN does for 1.0.4), answers with a
 * join accept, checks every uplink's MIC with the 32-bit FCnt and
 * decrypts it, and sends what the test queued: an ACK, MAC commands,
 * application data, in RX1 or RX2.
 *
 * Its numbers (channel frequencies, RX1 data rates and frequencies, the
 * downlink data rates) are written out here again rather than taken from
 * RegionUS915, so a wrong table in the region shows up as a missed
 * downlink. The frame functions (LoRaWanFrame) are shared; those are
 * checked against lora-packet in lorawan_frame_test.
 *
 * A join accept is the one thing the device never does: the server
 * *decrypts* to make it, so this file has its own AES-128 inverse cipher
 * (FIPS-197 section 5.3), checked against Appendix C.1 by the test.
 */
#pragma once
#include <cstdint>
#include <cstring>
#include <deque>
#include <vector>
#include "SimSX1276.h"
#include "LoRaWanFrame.h"
#include "AesCmac.h"

namespace sim {

// ---- AES-128 inverse cipher (decrypt) ------------------------------------
struct AesDecrypt {
	uint8_t sbox[256], inv[256], rk[176];
	static uint8_t xt(uint8_t x) { return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1B : 0)); }
	static uint8_t mul(uint8_t a, uint8_t b) {
		uint8_t r = 0;
		while (b) { if (b & 1) r ^= a; a = xt(a); b >>= 1; }
		return r;
	}
	explicit AesDecrypt(const uint8_t key[16]) {
		// The S-box from its definition: the inverse in GF(2^8), then the affine map.
		for (int x = 0; x < 256; ++x) {
			uint8_t i = 0;
			if (x) for (int y = 1; y < 256; ++y) if (mul((uint8_t)x, (uint8_t)y) == 1) { i = (uint8_t)y; break; }
			uint8_t s = i;
			for (int k = 1; k <= 4; ++k) s ^= (uint8_t)((i << k) | (i >> (8 - k)));
			sbox[x] = (uint8_t)(s ^ 0x63);
		}
		for (int x = 0; x < 256; ++x) inv[sbox[x]] = (uint8_t)x;
		memcpy(rk, key, 16);
		uint8_t rcon = 1;
		for (int i = 16; i < 176; i += 4) {
			uint8_t t[4] = {rk[i - 4], rk[i - 3], rk[i - 2], rk[i - 1]};
			if (i % 16 == 0) {
				const uint8_t f = t[0];
				t[0] = (uint8_t)(sbox[t[1]] ^ rcon); t[1] = sbox[t[2]]; t[2] = sbox[t[3]]; t[3] = sbox[f];
				rcon = xt(rcon);
			}
			for (int k = 0; k < 4; ++k) rk[i + k] = (uint8_t)(rk[i - 16 + k] ^ t[k]);
		}
	}
	void decrypt(const uint8_t in[16], uint8_t out[16]) const {
		uint8_t s[16];
		for (int i = 0; i < 16; ++i) s[i] = (uint8_t)(in[i] ^ rk[160 + i]);
		for (int round = 9; round >= 0; --round) {
			uint8_t t[16];
			for (int c = 0; c < 4; ++c)            // InvShiftRows
				for (int r = 0; r < 4; ++r) t[4 * ((c + r) % 4) + r] = s[4 * c + r];
			for (int i = 0; i < 16; ++i) s[i] = (uint8_t)(inv[t[i]] ^ rk[16 * round + i]);   // InvSubBytes, AddRoundKey
			if (round == 0) break;
			for (int c = 0; c < 4; ++c) {          // InvMixColumns
				const uint8_t a0 = s[4 * c], a1 = s[4 * c + 1], a2 = s[4 * c + 2], a3 = s[4 * c + 3];
				s[4 * c]     = (uint8_t)(mul(a0, 14) ^ mul(a1, 11) ^ mul(a2, 13) ^ mul(a3, 9));
				s[4 * c + 1] = (uint8_t)(mul(a0, 9) ^ mul(a1, 14) ^ mul(a2, 11) ^ mul(a3, 13));
				s[4 * c + 2] = (uint8_t)(mul(a0, 13) ^ mul(a1, 9) ^ mul(a2, 14) ^ mul(a3, 11));
				s[4 * c + 3] = (uint8_t)(mul(a0, 11) ^ mul(a1, 13) ^ mul(a2, 9) ^ mul(a3, 14));
			}
		}
		memcpy(out, s, 16);
	}
};

// ---- US915, written out again -----------------------------------------------
namespace us915 {
inline uint32_t upFreq(int ch) { return ch < 64 ? 902300000u + 200000u * (uint32_t)ch : 903000000u + 1600000u * (uint32_t)(ch - 64); }
// The radio tunes in steps of 61 Hz (32 MHz / 2^19): the nearest channel within 1 kHz.
inline int upChannel(uint32_t f) {
	for (int ch = 0; ch < 72; ++ch) { const int32_t d = (int32_t)(f - upFreq(ch)); if (d > -1000 && d < 1000) return ch; }
	return -1;
}
inline uint32_t rx1Freq(int ch) { return 923300000u + 600000u * (uint32_t)(ch % 8); }
// Uplink DR from SF and bandwidth code; downlink SF from DR.
inline int upDr(uint8_t sf, uint8_t bw) { if (bw == 9 && sf == 8) return 4; if (bw == 7 && sf >= 7 && sf <= 10) return 10 - sf; return -1; }
inline uint8_t downSf(int dr) { return (uint8_t)(12 - (dr - 8)); }   // DR8 SF12 ... DR13 SF7, all 500 kHz
inline int rx1Dr(int up, int off) {
	static const int t[5][4] = {{10, 9, 8, 8}, {11, 10, 9, 8}, {12, 11, 10, 9}, {13, 12, 11, 10}, {13, 13, 12, 11}};
	return t[up][off];
}
} // namespace us915

// ---- the server ------------------------------------------------------------------
struct LoRaWanServer {
	// The device, as registered.
	uint8_t devEui[8], joinEui[8], appKey[16];

	// Behaviour.
	bool answerJoins = true;
	int  dropJoins = 0;            // ignore this many join requests first
	bool joinInRx2 = false;
	bool replayJoinNonce = false;  // answer with the last JoinNonce again
	int  timingErrorMs = 0;        // send downlinks this much late (+) or early (-)
	uint32_t netId = 0x000013;     // TTN's
	uint32_t devAddrNext = 0x260B1200;
	std::vector<int> listen;       // uplink channels the gateway hears

	// The session.
	bool joined = false;
	uint32_t devAddr = 0, joinNonce = 0x000100;
	int lastDevNonce = -1;
	uint8_t nwkSKey[16], appSKey[16];
	uint32_t fCntUp = 0;           // the last accepted
	bool upSeen = false;
	uint32_t fCntDown = 0;         // the next to send
	int rx1Delay = 1, rx1Offset = 0, rx2Dr = 8;
	uint32_t rx2Freq = 923300000u;

	struct Up {
		uint32_t fCnt; uint8_t fCtrl; bool confirmed; int port; std::vector<uint8_t> fopts, payload;
		uint32_t freq; int ch, dr; uint32_t start, end; uint8_t paConfig, paDac;
	};
	std::vector<Up> ups;
	std::vector<uint16_t> devNonces;
	int joinRequests = 0, badMic = 0, rejectedNonce = 0;

	// What to send after the next uplink (one per uplink, in order).
	struct Plan {
		bool ack = true;               // ACK a confirmed uplink
		bool confirmed = false;        // a confirmed downlink
		bool fPending = false;
		std::vector<uint8_t> fopts;
		int port = -1;                 // -1: no port; 0: fopts go in the payload instead
		std::vector<uint8_t> payload;
		bool rx2 = false;
		int fCntJump = 0;              // skip this many FCntDown first
		bool badMic = false;
		uint32_t devAddrOverride = 0;
		bool replayLast = false;       // send the previous downlink again, bytes unchanged
	};
	std::deque<Plan> plans;
	bool alwaysAckConfirmed = true;    // an ACK even with no plan queued
	std::vector<uint8_t> lastDown;
	int downlinksSent = 0;

	// The gateway's radio and its schedule.
	Sx1276 gw;
	Air* air = nullptr;
	struct Tx { uint32_t at; uint32_t freq; int dr; std::vector<uint8_t> data; };
	std::vector<Tx> pending;

	void attach(Air& a) {
		air = &a;
		a.add(gw);
		gw.reg[sx1276::kRegOpMode] = sx1276::kOpLoRa | sx1276::kOpStandby;
		a.sniff = [this](const Sx1276& from, const Sx1276::Sent& p) { if (&from != &gw) hear(from, p); };
		if (listen.empty()) { for (int ch = 8; ch < 16; ++ch) listen.push_back(ch); listen.push_back(65); }
	}

	// Called every ms (before the devices): sends what is due.
	void tick(uint32_t now) {
		gw.tick(now);
		for (size_t i = 0; i < pending.size();) {
			if ((int32_t)(now - pending[i].at) >= 0) { send(pending[i]); pending.erase(pending.begin() + (long)i); }
			else ++i;
		}
	}

	void send(const Tx& t) {
		using namespace sx1276;
		Sx1276& c = gw;
		const uint32_t frf = lora::frf(t.freq);
		c.write(kRegFrfMsb, (uint8_t)(frf >> 16)); c.write(kRegFrfMsb + 1, (uint8_t)(frf >> 8)); c.write(kRegFrfMsb + 2, (uint8_t)frf);
		const uint8_t sf = us915::downSf(t.dr);
		c.write(kRegModemConfig1, (uint8_t)((uint8_t)lora::Bw::Bw500k << 4 | 1 << 1));
		c.write(kRegModemConfig2, (uint8_t)(sf << 4));                 // no payload CRC on downlinks
		c.write(kRegModemConfig3, (uint8_t)(lora::lowDataRateOptimize(sf, lora::Bw::Bw500k) ? kLdro : 0));
		c.write(kRegPreambleMsb, 0); c.write(kRegPreambleMsb + 1, 8);
		c.write(kRegSyncWord, kSyncWordLoRaWan);
		c.write(kRegInvertIq, kInvertIqTx);
		c.write(kRegInvertIq2, kInvertIq2Inverted);
		c.write(kRegFifoTxBaseAddr, 0);
		c.write(kRegFifoAddrPtr, 0);
		for (uint8_t b : t.data) c.write(kRegFifo, b);
		c.write(kRegPayloadLength, (uint8_t)t.data.size());
		c.write(kRegOpMode, kOpLoRa | kOpTx);
		++downlinksSent;
	}

	void schedule(uint32_t at, uint32_t freq, int dr, const std::vector<uint8_t>& data) {
		pending.push_back(Tx{(uint32_t)((int32_t)at + timingErrorMs), freq, dr, data});
	}

	void hear(const Sx1276& from, const Sx1276::Sent& p) {
		const int ch = us915::upChannel(p.freqHz);
		const int dr = us915::upDr(p.sf, p.bw);
		if (ch < 0 || dr < 0 || p.invertIq || from.reg[sx1276::kRegSyncWord] != sx1276::kSyncWordLoRaWan) return;
		bool heard = false;
		for (int l : listen) heard |= (l == ch);
		if (!heard || p.data.empty()) return;
		if ((ch < 64) != (dr < 4)) return;   // a 125 kHz channel at DR4, or the reverse
		const uint8_t mt = p.data[0] >> 5;
		if (mt == lorawan::mtype_join_request) joinRequest(p, ch, dr);
		else if (mt == lorawan::mtype_unconfirmed_up || mt == lorawan::mtype_confirmed_up) uplink(p, ch, dr);
	}

	void joinRequest(const Sx1276::Sent& p, int ch, int dr) {
		const std::vector<uint8_t>& d = p.data;
		if (d.size() != 23) return;
		uint8_t tag[16];
		AesCmac::compute(appKey, d.data(), 19, tag);
		if (memcmp(tag, d.data() + 19, 4) != 0) { ++badMic; return; }
		for (int i = 0; i < 8; ++i) if (d[1 + i] != joinEui[7 - i] || d[9 + i] != devEui[7 - i]) return;
		++joinRequests;
		const uint16_t nonce = (uint16_t)(d[17] | (d[18] << 8));
		devNonces.push_back(nonce);
		if ((int)nonce <= lastDevNonce) { ++rejectedNonce; return; }   // TTN: DevNonce must count up
		lastDevNonce = nonce;
		if (dropJoins > 0) { --dropJoins; return; }
		if (!answerJoins) return;
		if (!replayJoinNonce) ++joinNonce;
		devAddr = devAddrNext++;
		// Plain: MHDR JoinNonce NetID DevAddr DLSettings RxDelay CFList MIC.
		std::vector<uint8_t> f = {lorawan::mhdr(lorawan::mtype_join_accept)};
		auto put = [&](uint32_t v, int n) { for (int i = 0; i < n; ++i) f.push_back((uint8_t)(v >> (8 * i))); };
		put(joinNonce, 3); put(netId, 3); put(devAddr, 4);
		f.push_back((uint8_t)(rx1Offset << 4 | rx2Dr));
		f.push_back((uint8_t)rx1Delay);
		// CFList type 1: sub-band 2 (channels 8-15, 65).
		const uint8_t cf[16] = {0x00, 0xFF, 0, 0, 0, 0, 0, 0, 0x02, 0x00, 0, 0, 0, 0, 0, 0x01};
		f.insert(f.end(), cf, cf + 16);
		AesCmac::compute(appKey, f.data(), f.size(), tag);
		f.insert(f.end(), tag, tag + 4);
		std::vector<uint8_t> wire = f;
		AesDecrypt aes(appKey);
		for (size_t off = 1; off < wire.size(); off += 16) aes.decrypt(&f[off], &wire[off]);
		lorawan::deriveSessionKeys(appKey, joinNonce, netId, nonce, nwkSKey, appSKey);
		joined = true;
		upSeen = false;
		fCntUp = 0;
		fCntDown = 0;
		if (joinInRx2) schedule(p.end + 6000, 923300000u, 8, wire);
		else schedule(p.end + 5000, us915::rx1Freq(ch), us915::rx1Dr(dr, 0), wire);
	}

	void uplink(const Sx1276::Sent& p, int ch, int dr) {
		if (!joined) return;
		std::vector<uint8_t> f = p.data;
		lorawan::Downlink h;   // the header layout is the same both ways
		if (f.size() < 12) return;
		const uint32_t addr = (uint32_t)f[1] | (uint32_t)f[2] << 8 | (uint32_t)f[3] << 16 | (uint32_t)f[4] << 24;
		if (addr != devAddr) return;
		const uint16_t fc16 = (uint16_t)(f[6] | f[7] << 8);
		uint32_t fc = (fCntUp & 0xFFFF0000u) | fc16;
		if (upSeen && fc < fCntUp) fc += 0x10000u;
		uint8_t mic[4];
		lorawan::dataMic(nwkSKey, lorawan::kDirUp, addr, fc, f.data(), (uint8_t)(f.size() - 4), mic);
		if (memcmp(mic, f.data() + f.size() - 4, 4) != 0) { ++badMic; return; }
		const bool repeat = upSeen && fc == fCntUp;   // a retransmission
		fCntUp = fc;
		upSeen = true;
		Up u;
		u.fCnt = fc; u.fCtrl = f[5]; u.confirmed = (f[0] >> 5) == lorawan::mtype_confirmed_up;
		u.freq = p.freqHz; u.ch = ch; u.dr = dr; u.start = p.start; u.end = p.end;
		u.paConfig = p.paConfig; u.paDac = p.paDac;
		const uint8_t foptsLen = f[5] & 0x0F;
		u.fopts.assign(f.begin() + 8, f.begin() + 8 + foptsLen);
		size_t n = 8 + foptsLen;
		const size_t body = f.size() - 4;
		u.port = -1;
		if (n < body) {
			u.port = f[n++];
			u.payload.assign(f.begin() + (long)n, f.begin() + (long)body);
			lorawan::cryptPayload(u.port == 0 ? nwkSKey : appSKey, lorawan::kDirUp, addr, fc, u.payload.data(), (uint8_t)u.payload.size());
		}
		ups.push_back(u);
		(void)repeat;
		(void)h;

		Plan pl;
		const bool havePlan = !plans.empty();
		if (havePlan) { pl = plans.front(); plans.pop_front(); }
		else if (!(u.confirmed && alwaysAckConfirmed)) return;
		std::vector<uint8_t> d;
		if (pl.replayLast) {
			d = lastDown;
		} else {
			fCntDown += (uint32_t)pl.fCntJump;
			const uint32_t a = pl.devAddrOverride ? pl.devAddrOverride : devAddr;
			d.push_back(lorawan::mhdr(pl.confirmed ? lorawan::mtype_confirmed_down : lorawan::mtype_unconfirmed_down));
			for (int i = 0; i < 4; ++i) d.push_back((uint8_t)(a >> (8 * i)));
			const bool inPayload = pl.port == 0;
			d.push_back((uint8_t)((pl.ack && u.confirmed ? lorawan::kFCtrlAck : 0) | (pl.fPending ? lorawan::kFCtrlFPending : 0) |
			                      (inPayload ? 0 : pl.fopts.size())));
			d.push_back((uint8_t)fCntDown); d.push_back((uint8_t)(fCntDown >> 8));
			if (!inPayload) d.insert(d.end(), pl.fopts.begin(), pl.fopts.end());
			if (pl.port >= 0) {
				d.push_back((uint8_t)pl.port);
				std::vector<uint8_t> pay = inPayload ? pl.fopts : pl.payload;
				lorawan::cryptPayload(inPayload ? nwkSKey : appSKey, lorawan::kDirDown, a, fCntDown, pay.data(), (uint8_t)pay.size());
				d.insert(d.end(), pay.begin(), pay.end());
			}
			uint8_t m[4];
			lorawan::dataMic(nwkSKey, lorawan::kDirDown, a, fCntDown, d.data(), (uint8_t)d.size(), m);
			if (pl.badMic) m[0] ^= 1;
			d.insert(d.end(), m, m + 4);
			++fCntDown;
			lastDown = d;
		}
		if (pl.rx2) schedule(p.end + (uint32_t)rx1Delay * 1000 + 1000, rx2Freq, rx2Dr, d);
		else schedule(p.end + (uint32_t)rx1Delay * 1000, us915::rx1Freq(ch), us915::rx1Dr(dr, rx1Offset), d);
	}
};

} // namespace sim
