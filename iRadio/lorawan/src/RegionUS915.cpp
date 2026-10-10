/*
 * RegionUS915.cpp
 *
 *  See RegionUS915.h.
 */

#include "RegionUS915.h"
#include <string.h>

namespace lorawan {

namespace {

// Uplink DR0-4 and downlink DR8-13 (LoRaMac-node's DataratesUS915/BandwidthsUS915).
const uint8_t kSf[14] = {10, 9, 8, 7, 8, 0, 0, 0, 12, 11, 10, 9, 8, 7};
// N: FOpts + FRMPayload (MaxPayloadOfDatarateUS915, the 400 ms dwell time limit).
const uint8_t kMaxN[14] = {11, 53, 125, 242, 242, 0, 0, 0, 53, 129, 242, 242, 242, 242};
// RX1 data rate by uplink DR and RX1DROffset (DatarateOffsetsUS915).
const uint8_t kRx1Dr[5][4] = {
	{10, 9, 8, 8},
	{11, 10, 9, 8},
	{12, 11, 10, 9},
	{13, 12, 11, 10},
	{13, 13, 12, 11},
};

const uint32_t kRx1First = 923300000u, kRx1Last = 927500000u, kRx1Step = 600000u;

} // namespace

RegionUS915::RegionUS915(uint8_t subBand) : _subBand(subBand > 8 ? 0 : subBand) {
	if (_subBand == 0) {
		_default[0] = _default[1] = _default[2] = _default[3] = 0xFFFF;
		_default[4] = 0x00FF;
	} else {
		memset(_default, 0, sizeof _default);
		const uint8_t b = (uint8_t)(_subBand - 1);   // channels 8b..8b+7 and 64+b
		_default[b >> 1] = (b & 1) ? 0xFF00 : 0x00FF;
		_default[4] = (uint16_t)(1u << b);
	}
	reset();
}

void RegionUS915::reset() {
	memcpy(_mask, _default, sizeof _mask);
	memcpy(_remaining, _mask, sizeof _remaining);
	_joinGroup = 0;
}

uint32_t RegionUS915::channelFrequency(uint8_t ch) {
	return ch < 64 ? 902300000u + 200000u * ch : 903000000u + 1600000u * (uint32_t)(ch - 64);
}

uint8_t RegionUS915::count(const uint16_t* m, uint8_t from, uint8_t to) {
	uint8_t n = 0;
	for (uint8_t i = from; i < to; ++i)
		for (uint16_t v = m[i]; v; v &= (uint16_t)(v - 1)) ++n;
	return n;
}

uint8_t RegionUS915::enabledCount125() const { return count(_mask, 0, 4); }

bool RegionUS915::anyCarries(const uint16_t* m, uint8_t dr) const {
	if (dr <= 3) return count(m, 0, 4) > 0;
	if (dr == 4) return (m[4] & 0xFF) != 0;
	return false;
}

bool RegionUS915::txDataRate(uint8_t dr, lora::Config* c) const {
	if (dr > 4) return false;
	c->sf = kSf[dr];
	c->bw = dr == 4 ? lora::Bw::Bw500k : lora::Bw::Bw125k;
	return true;
}

bool RegionUS915::rxDataRate(uint8_t dr, lora::Config* c) const {
	if (dr < 8 || dr > 13) return false;
	c->sf = kSf[dr];
	c->bw = lora::Bw::Bw500k;
	return true;
}

uint8_t RegionUS915::maxPayload(uint8_t dr) const { return dr < 14 ? kMaxN[dr] : 0; }

bool RegionUS915::nextChannel(uint8_t dr, bool joining, uint32_t rnd, uint8_t* channel, uint32_t* freqHz) {
	if (dr > 4) return false;
	// Every enabled channel used: start the round again.
	if (count(_remaining, 0, 4) == 0) {
		for (uint8_t i = 0; i < 4; ++i) _remaining[i] = _mask[i];
		_joinGroup = 0;
	}
	if (dr == 4 && (_remaining[4] & 0xFF) == 0) _remaining[4] = _mask[4];
	if (!anyCarries(_remaining, dr)) return false;

	uint8_t ch = 0;
	if (dr == 4 && joining) {
		while (!((_remaining[4] >> ch) & 1)) ++ch;          // the lowest left
		ch = (uint8_t)(64 + ch);
	} else if (dr == 4) {
		uint8_t pick = (uint8_t)(rnd % count(_remaining, 4, 5));
		for (uint8_t i = 0; i < 8; ++i)
			if ((_remaining[4] >> i) & 1) { if (pick == 0) { ch = (uint8_t)(64 + i); break; } --pick; }
	} else if (joining) {
		// The next 8-channel group with a channel left, a channel at random in it.
		for (uint8_t tries = 0; tries < 8; ++tries) {
			const uint8_t g = _joinGroup;
			_joinGroup = (uint8_t)((_joinGroup + 1) & 7);
			const uint8_t bits = (uint8_t)(_remaining[g >> 1] >> ((g & 1) * 8));
			if (!bits) continue;
			uint8_t n = 0;
			for (uint8_t v = bits; v; v &= (uint8_t)(v - 1)) ++n;
			uint8_t pick = (uint8_t)(rnd % n);
			for (uint8_t i = 0; i < 8; ++i)
				if ((bits >> i) & 1) { if (pick == 0) { ch = (uint8_t)(g * 8 + i); break; } --pick; }
			break;
		}
	} else {
		uint8_t pick = (uint8_t)(rnd % count(_remaining, 0, 4));
		for (uint8_t i = 0; i < 64; ++i)
			if ((_remaining[i >> 4] >> (i & 15)) & 1) { if (pick == 0) { ch = i; break; } --pick; }
	}
	_remaining[ch >> 4] &= (uint16_t)~(1u << (ch & 15));
	*channel = ch;
	*freqHz = channelFrequency(ch);
	return true;
}

uint32_t RegionUS915::rx1Frequency(uint8_t channel, uint32_t upFreqHz) const {
	(void)upFreqHz;
	return kRx1First + kRx1Step * (uint32_t)(channel % 8);
}

uint8_t RegionUS915::rx1DataRate(uint8_t upDr, uint8_t rx1DrOffset) const {
	if (upDr > 4) upDr = 4;
	if (rx1DrOffset > 3) rx1DrOffset = 3;
	return kRx1Dr[upDr][rx1DrOffset];
}

int8_t RegionUS915::eirpDbm(uint8_t txPower, uint8_t dr) const {
	if (txPower > 14) txPower = 14;
	if (dr == 4) { if (txPower < 2) txPower = 2; }            // 26 dBm
	else if (enabledCount125() < 50 && txPower < 5) txPower = 5;   // 20 dBm
	return (int8_t)(30 - 2 * txPower);
}

uint8_t RegionUS915::linkAdrReq(const uint8_t* cmds, uint8_t len, bool adrOn, AdrState* st, uint8_t* consumed) {
	uint16_t m[5];
	memcpy(m, _mask, sizeof m);
	uint8_t dr = 0, power = 0, nbTrans = 0, n = 0;
	while (n + 5 <= len && cmds[n] == 0x03) {
		dr = (uint8_t)(cmds[n + 1] >> 4);
		power = (uint8_t)(cmds[n + 1] & 0x0F);
		const uint16_t chMask = (uint16_t)(cmds[n + 2] | (cmds[n + 3] << 8));
		const uint8_t cntl = (uint8_t)((cmds[n + 4] >> 4) & 0x07);
		nbTrans = (uint8_t)(cmds[n + 4] & 0x0F);
		n = (uint8_t)(n + 5);
		if (cntl == 6 || cntl == 7) {
			const uint16_t v = cntl == 6 ? 0xFFFF : 0x0000;
			m[0] = m[1] = m[2] = m[3] = v;
			m[4] = (uint16_t)(chMask & 0x00FF);
		} else if (cntl == 5) {
			// Bits 0-7: a block of eight 125 kHz channels and the 500 kHz channel 64+i.
			m[4] = 0;
			for (uint8_t i = 0; i < 8; ++i) {
				const bool on = (chMask >> i) & 1;
				const uint16_t half = (i & 1) ? 0xFF00 : 0x00FF;
				if (on) { m[i >> 1] |= half; m[4] |= (uint16_t)(1u << i); }
				else    { m[i >> 1] &= (uint16_t)~half; }
			}
		} else {
			m[cntl] = cntl == 4 ? (uint16_t)(chMask & 0x00FF) : chMask;
		}
	}
	*consumed = n;
	if (n == 0) return 0;

	uint8_t status = 0x07;
	if (!adrOn) { dr = st->dr; power = st->txPower; nbTrans = st->nbTrans; }
	if (dr == 0x0F) dr = st->dr;
	if (power == 0x0F) power = st->txPower;
	// FCC 15.247: at least two 125 kHz channels for DR0-3.
	if (dr < 4 && count(m, 0, 4) < 2) status &= 0xFE;
	if (dr > 4 || !anyCarries(m, dr)) status &= 0xFD;
	if (power > 14) status &= 0xFB;
	if (status == 0x07) {
		memcpy(_mask, m, sizeof _mask);
		for (uint8_t i = 0; i < 4; ++i) _remaining[i] &= _mask[i];
		_remaining[4] = _mask[4];
		st->dr = dr;
		st->txPower = power;
		st->nbTrans = nbTrans ? nbTrans : 1;
	}
	return status;
}

uint8_t RegionUS915::rxParamSetupStatus(uint8_t rx1DrOffset, uint8_t rx2Dr, uint32_t freqHz) const {
	uint8_t status = 0x07;
	if (freqHz < kRx1First || freqHz > kRx1Last || (freqHz - kRx1First) % kRx1Step != 0) status &= 0xFE;
	if (rx2Dr < 8 || rx2Dr > 13) status &= 0xFD;
	if (rx1DrOffset > 3) status &= 0xFB;
	return status;
}

void RegionUS915::applyCfList(const uint8_t cfList[16]) {
	if (cfList[15] != 0x01) return;   // CFListType 1: channel masks
	for (uint8_t i = 0; i < 5; ++i) {
		_mask[i] = (uint16_t)(cfList[2 * i] | (cfList[2 * i + 1] << 8));
		if (i == 4) _mask[i] &= 0x00FF;
		_remaining[i] &= _mask[i];
	}
}

uint8_t RegionUS915::nextLowerDr(uint8_t dr) const {
	while (dr > 0) {
		--dr;
		if (anyCarries(_mask, dr)) return dr;
	}
	return 0;
}

void RegionUS915::enableDefaultChannels() {
	memcpy(_mask, _default, sizeof _mask);
	for (uint8_t i = 0; i < 5; ++i) _remaining[i] &= _mask[i];
}

void RegionUS915::saveState(uint8_t* p) const {
	for (uint8_t i = 0; i < 5; ++i) { p[2 * i] = (uint8_t)_mask[i]; p[2 * i + 1] = (uint8_t)(_mask[i] >> 8); }
}

bool RegionUS915::loadState(const uint8_t* p, uint8_t len) {
	if (len != stateSize()) return false;
	for (uint8_t i = 0; i < 5; ++i) _mask[i] = (uint16_t)(p[2 * i] | (p[2 * i + 1] << 8));
	_mask[4] &= 0x00FF;
	memcpy(_remaining, _mask, sizeof _remaining);
	return true;
}

} // namespace lorawan
