/*
 * LoRaWanMac.cpp
 *
 *  See LoRaWanMac.h. The behaviour follows LoRaWAN 1.0.4 as Semtech's
 *  LoRaMac-node v4.7.0 implements it (LoRaMac.c, LoRaMacAdr.c,
 *  RegionCommon.c); where this differs, the comment says so.
 */

#include "LoRaWanMac.h"
#include <string.h>
#include "DebugLog.h"

namespace lorawan {

namespace {

// Uplink MAC commands (answers and requests): CID and length with the CID.
uint8_t upCmdLen(uint8_t cid) {
	switch (cid) {
	case 0x02: return 1;   // LinkCheckReq
	case 0x03: return 2;   // LinkADRAns
	case 0x04: return 1;   // DutyCycleAns
	case 0x05: return 2;   // RXParamSetupAns
	case 0x06: return 3;   // DevStatusAns
	case 0x08: return 1;   // RXTimingSetupAns
	case 0x0D: return 1;   // DeviceTimeReq
	default:   return 0;
	}
}

uint16_t crc16(const uint8_t* p, uint16_t n) {   // CRC-16/CCITT-FALSE
	uint16_t c = 0xFFFF;
	for (uint16_t i = 0; i < n; ++i) {
		c ^= (uint16_t)(p[i] << 8);
		for (uint8_t b = 0; b < 8; ++b) c = (c & 0x8000) ? (uint16_t)((c << 1) ^ 0x1021) : (uint16_t)(c << 1);
	}
	return c;
}

const uint8_t kMagic0 = 'L', kMagic1 = 'W', kVersion = 1;
const uint8_t kRegionStateMax = 16;
const uint32_t kTxStartTimeoutMs = 2000;   // the radio never took the request
const uint32_t kRadioGuardMs = 4000;       // beyond the radio's own timeouts

} // namespace

MacParam defaultMacParam() {
	MacParam p;
	memset(&p, 0, sizeof p);
	p.dataRate = 0;
	p.adr = true;
	p.antennaGainDb = 2;
	p.confirmedTries = 1;
	p.saveEvery = 16;
	p.randomSeed = 0x2545F491u;
	p.rxErrorMs = 10;
	p.minRxSymbols = 6;
	p.radioWakeMs = 3;
	p.battery = 255;
	return p;
}

Mac::Mac(lora::iLoRaRadio& radio, Region& region, iSessionStore& store, const MacParam& param)
	: _radio(radio), _region(region), _store(store), _p(param), _rng(param.randomSeed ? param.randomSeed : 1u) {
	if (_p.saveEvery == 0) _p.saveEvery = 1;
	if (_p.confirmedTries == 0) _p.confirmedTries = 1;
	memset(_nwkSKey, 0, sizeof _nwkSKey);
	memset(_appSKey, 0, sizeof _appSKey);
	resetSessionState();
}

uint32_t Mac::rnd() {   // xorshift32
	_rng ^= _rng << 13;
	_rng ^= _rng >> 17;
	_rng ^= _rng << 5;
	return _rng;
}

uint32_t Mac::msToTicks(uint32_t ms) const {
	return (uint32_t)((uint64_t)ms * _radio.ticksPerSecond() / 1000u);
}

void Mac::rxWindow(uint32_t symbolUs, uint8_t minSymbols, uint32_t errorMs, uint32_t wakeMs,
                   uint16_t* symbols, int32_t* offsetMs) {
	// RegionCommonComputeRxWindowParameters: listen for the preamble's
	// minimum plus twice the clock error, centred on the preamble.
	const int64_t t = symbolUs;
	int64_t n = ((2 * (int64_t)minSymbols - 8) * t + 2 * (int64_t)errorMs * 1000 + t - 1) / t;
	if (n < minSymbols) n = minSymbols;
	if (n > 1023) n = 1023;
	*symbols = (uint16_t)n;
	const int64_t half = (n * t + 1) / 2;
	const int64_t us = 4 * t - half - (int64_t)wakeMs * 1000;
	// LoRaMac-node's DIV_CEIL, (a + b - 1) / b, in C's truncating division:
	// a ceiling for a positive offset, one ms nearer zero for a negative one.
	*offsetMs = (int32_t)((us + 999) / 1000);
}

// ---- the session -----------------------------------------------------------

void Mac::resetSessionState() {
	_joined = false;
	_devAddr = 0;
	memset(_nwkSKey, 0, sizeof _nwkSKey);
	memset(_appSKey, 0, sizeof _appSKey);
	_fCntUp = _fCntUpSaved = 0;
	_fCntDown = 0;
	_downSeen = false;
	_rx1DrOffset = 0;
	_rx2Dr = _region.rx2DefaultDataRate();
	_rx2Freq = _region.rx2DefaultFrequency();
	_rx1Delay = 1;
	_maxDutyCycle = 0;
	_adr.dr = _p.dataRate < _region.minTxDr() ? _region.minTxDr()
	        : _p.dataRate > _region.maxTxDr() ? _region.maxTxDr() : _p.dataRate;
	_adr.txPower = _region.defaultTxPower();
	_adr.nbTrans = 1;
	_adrAckCnt = 0;
	_adrAckReq = false;
	_ansLen = _stickyLen = 0;
	_ackDownlink = false;
	_region.reset();
}

void Mac::encode(uint8_t* p) const {
	uint16_t n = 0;
	auto u8 = [&](uint8_t v) { p[n++] = v; };
	auto u16 = [&](uint16_t v) { u8((uint8_t)v); u8((uint8_t)(v >> 8)); };
	auto u24 = [&](uint32_t v) { u16((uint16_t)v); u8((uint8_t)(v >> 16)); };
	auto u32 = [&](uint32_t v) { u16((uint16_t)v); u16((uint16_t)(v >> 16)); };
	u8(kMagic0); u8(kMagic1); u8(kVersion);
	u8((uint8_t)((_joined ? 1 : 0) | (_downSeen ? 2 : 0) | (_haveJoinNonce ? 4 : 0)));
	for (uint8_t i = 0; i < 8; ++i) u8(_p.devEui[i]);
	u16(_devNonce);
	u24(_joinNonce);
	u32(_devAddr);
	for (uint8_t i = 0; i < 16; ++i) u8(_nwkSKey[i]);
	for (uint8_t i = 0; i < 16; ++i) u8(_appSKey[i]);
	u32(_fCntUpSaved);
	u32(_fCntDown);
	u8(_rx1DrOffset); u8(_rx2Dr); u32(_rx2Freq); u8(_rx1Delay);
	u8(_adr.dr); u8(_adr.txPower); u8(_adr.nbTrans); u8(_maxDutyCycle);
	const uint8_t rl = _region.stateSize() <= kRegionStateMax ? _region.stateSize() : 0;
	u8(rl);
	uint8_t region[kRegionStateMax];
	memset(region, 0, sizeof region);
	if (rl) _region.saveState(region);
	for (uint8_t i = 0; i < kRegionStateMax; ++i) u8(region[i]);
	u16(crc16(p, n));
}

bool Mac::decode(const uint8_t* p) {
	if (p[0] != kMagic0 || p[1] != kMagic1 || p[2] != kVersion) return false;
	if (crc16(p, kSessionBytes - 2) != (uint16_t)(p[kSessionBytes - 2] | (p[kSessionBytes - 1] << 8))) return false;
	if (memcmp(p + 4, _p.devEui, 8) != 0) return false;   // another device's
	uint16_t n = 12;
	auto u8 = [&]() { return p[n++]; };
	auto u16 = [&]() { uint16_t v = p[n]; v = (uint16_t)(v | (p[n + 1] << 8)); n = (uint16_t)(n + 2); return v; };
	auto u24 = [&]() { uint32_t v = u16(); return v | ((uint32_t)u8() << 16); };
	auto u32 = [&]() { uint32_t v = u16(); return v | ((uint32_t)u16() << 16); };
	const uint8_t flags = p[3];
	_devNonce = u16();
	_joinNonce = u24();
	_haveJoinNonce = (flags & 4) != 0;
	_joined = (flags & 1) != 0;
	_downSeen = (flags & 2) != 0;
	_devAddr = u32();
	for (uint8_t i = 0; i < 16; ++i) _nwkSKey[i] = u8();
	for (uint8_t i = 0; i < 16; ++i) _appSKey[i] = u8();
	_fCntUpSaved = u32();
	_fCntUp = _fCntUpSaved;   // never below anything that may have been sent
	_fCntDown = u32();
	_rx1DrOffset = u8(); _rx2Dr = u8(); _rx2Freq = u32(); _rx1Delay = u8();
	_adr.dr = u8(); _adr.txPower = u8(); _adr.nbTrans = u8(); _maxDutyCycle = u8();
	const uint8_t rl = u8();
	if (_joined && rl && !_region.loadState(p + n, rl)) _region.reset();
	return true;
}

bool Mac::saveSession() {
	uint8_t buf[kSessionBytes];
	encode(buf);
	return _store.save(buf, kSessionBytes);
}

void Mac::begin(uint32_t nowMs) {
	(void)nowMs;
	resetSessionState();
	_devNonce = 0;
	_haveJoinNonce = false;
	_joinNonce = 0;
	uint8_t buf[kSessionBytes];
	if (_store.load(buf, kSessionBytes) && decode(buf)) {
		DBG_EVENT("lwmac", "session", _joined ? 1 : 0, (int32_t)_devNonce, (int32_t)_fCntUp);
		if (!_joined) {
			const uint16_t nonce = _devNonce;
			const bool have = _haveJoinNonce;
			const uint32_t jn = _joinNonce;
			resetSessionState();
			_devNonce = nonce; _haveJoinNonce = have; _joinNonce = jn;
		}
	} else {
		resetSessionState();
	}
	memset(buf, 0, sizeof buf);
	_st = St::Idle;
	_job = Job::None;
}

// ---- requests -----------------------------------------------------------------

bool Mac::join(uint16_t maxTries) {
	if (_st != St::Idle) return false;
	const uint16_t nonce = _devNonce;
	const bool have = _haveJoinNonce;
	const uint32_t jn = _joinNonce;
	resetSessionState();   // a join starts a new session
	_devNonce = nonce; _haveJoinNonce = have; _joinNonce = jn;
	_job = Job::Join;
	_joinTries = 0;
	_joinMax = maxTries;
	_joinStartMs = _stateMs;
	_waitUntilMs = _stateMs;
	_st = St::Wait;
	return true;
}

uint8_t Mac::maxPayloadNow() const {
	return _region.maxPayload(_adr.dr);
}

bool Mac::send(uint8_t port, const uint8_t* data, uint8_t len, bool confirmed) {
	if (_st != St::Idle || !_joined) return false;
	if (port == 0 || port > 223) return false;
	if (len > maxPayloadNow() || len > kMaxPayload || (len && !data)) return false;
	// FCntUp: the saved value is a ceiling; move it on before passing it.
	if (_fCntUp >= _fCntUpSaved) {
		const uint32_t old = _fCntUpSaved;
		_fCntUpSaved = _fCntUp + _p.saveEvery;
		if (!saveSession()) {
			_fCntUpSaved = old;
			pushSimple(mac_ev_fault, mac_fault_store);
			return false;
		}
	}
	if (len) memcpy(_data, data, len);
	_port = port;
	_len = len;
	_confirmed = confirmed;
	_acked = false;
	_gotDownlink = false;
	_fPendingSeen = false;
	_tries = 0;
	_frameFCnt = _fCntUp++;
	adrNext();
	_triesMax = _adr.nbTrans;
	if (confirmed && _triesMax < _p.confirmedTries) _triesMax = _p.confirmedTries;
	_frameLen = 0;
	_job = Job::Data;
	_waitUntilMs = _stateMs;
	_st = St::Wait;
	return true;
}

// LoRaMacAdrCalcNext: the ADR acknowledgement counter and the backoff.
void Mac::adrNext() {
	if (!_p.adr) { _adrAckReq = false; return; }
	if (_adrAckCnt < 0xFFFFFFFFu) ++_adrAckCnt;
	_adrAckReq = _adrAckCnt >= kAdrAckLimit;
	if (_adrAckCnt >= (uint32_t)kAdrAckLimit + kAdrAckDelay) _adr.txPower = _region.defaultTxPower();
	if (_adrAckCnt >= (uint32_t)kAdrAckLimit + 2u * kAdrAckDelay &&
	    (_adrAckCnt - kAdrAckLimit) % kAdrAckDelay == 0) {
		if (_adr.dr == _region.minTxDr()) {
			_region.enableDefaultChannels();
			_adr.nbTrans = 1;
		}
		_adr.dr = _region.nextLowerDr(_adr.dr);
		DBG_EVENT("lwmac", "adr-backoff", (int32_t)_adrAckCnt, _adr.dr);
	}
}

// ---- events -------------------------------------------------------------------

void Mac::push(const MacEvent& e) {
	if (_evCount == kEvents) {
		++_dropped;
		DBG_FAULT("lwmac", "event-dropped", (int32_t)e.kind);
		if (e.kind == mac_ev_downlink) _downHeld = false;
		return;
	}
	_ev[(uint8_t)((_evHead + _evCount) % kEvents)] = e;
	++_evCount;
}

void Mac::pushSimple(MacEventKind k, uint8_t fault) {
	MacEvent e;
	memset(&e, 0, sizeof e);
	e.kind = k;
	e.fault = fault;
	push(e);
}

bool Mac::takeEvent(MacEvent* e, uint8_t* buf, uint8_t cap) {
	if (_evCount == 0) return false;
	*e = _ev[_evHead];
	_evHead = (uint8_t)((_evHead + 1) % kEvents);
	--_evCount;
	if (e->kind == mac_ev_downlink) {
		if (buf && cap) memcpy(buf, _down, e->len < cap ? e->len : cap);
		_downHeld = false;
	}
	return true;
}

// ---- MAC commands --------------------------------------------------------------

void Mac::addAnswer(uint8_t cid, const uint8_t* v, uint8_t n) {
	if (_ansLen + 1 + n > kMaxFOpts) { DBG_FAULT("lwmac", "answers-full", cid); return; }
	_ans[_ansLen++] = cid;
	for (uint8_t i = 0; i < n; ++i) _ans[_ansLen++] = v[i];
}

void Mac::addSticky(uint8_t cid, const uint8_t* v, uint8_t n) {
	// One of each: a newer request replaces the answer to an older one.
	for (uint8_t i = 0; i < _stickyLen;) {
		const uint8_t l = upCmdLen(_sticky[i]);
		if (_sticky[i] == cid) {
			memmove(_sticky + i, _sticky + i + l, (size_t)(_stickyLen - i - l));
			_stickyLen = (uint8_t)(_stickyLen - l);
		} else {
			i = (uint8_t)(i + l);
		}
	}
	if (_stickyLen + 1 + n > kMaxFOpts) return;
	_sticky[_stickyLen++] = cid;
	for (uint8_t i = 0; i < n; ++i) _sticky[_stickyLen++] = v[i];
}

void Mac::handleCommands(const uint8_t* p, uint8_t len, int8_t snr) {
	bool adrBlockSeen = false;
	uint8_t i = 0;
	while (i < len) {
		const uint8_t cid = p[i];
		const uint8_t left = (uint8_t)(len - i - 1);
		const uint8_t* a = p + i + 1;
		switch (cid) {
		case 0x02: {   // LinkCheckAns
			if (left < 2) return;
			MacEvent e;
			memset(&e, 0, sizeof e);
			e.kind = mac_ev_link_check;
			e.margin = a[0];
			e.gateways = a[1];
			e.snrDb = snr;
			push(e);
			i = (uint8_t)(i + 3);
			break;
		}
		case 0x03: {   // LinkADRReq: a block of them, one answer each
			uint8_t used = 0;
			if (adrBlockSeen) {   // only the first block counts; skip the others
				while (i + 5 <= len && p[i] == 0x03) i = (uint8_t)(i + 5);
				if (i < len && p[i] == 0x03) return;
				break;
			}
			adrBlockSeen = true;
			const uint8_t status = _region.linkAdrReq(p + i, (uint8_t)(len - i), _p.adr, &_adr, &used);
			if (used == 0) return;
			for (uint8_t k = 0; k < used / 5; ++k) addAnswer(0x03, &status, 1);
			DBG_EVENT("lwmac", "linkadr", status, _adr.dr, _adr.txPower, _adr.nbTrans);
			i = (uint8_t)(i + used);
			break;
		}
		case 0x04:   // DutyCycleReq
			if (left < 1) return;
			_maxDutyCycle = (uint8_t)(a[0] & 0x0F);
			addAnswer(0x04, nullptr, 0);
			i = (uint8_t)(i + 2);
			break;
		case 0x05: {   // RXParamSetupReq
			if (left < 4) return;
			const uint8_t off = (uint8_t)((a[0] >> 4) & 0x07), dr = (uint8_t)(a[0] & 0x0F);
			const uint32_t f = ((uint32_t)a[1] | ((uint32_t)a[2] << 8) | ((uint32_t)a[3] << 16)) * 100u;
			const uint8_t status = _region.rxParamSetupStatus(off, dr, f);
			if (status == 0x07) { _rx1DrOffset = off; _rx2Dr = dr; _rx2Freq = f; }
			addSticky(0x05, &status, 1);
			i = (uint8_t)(i + 5);
			break;
		}
		case 0x06: {   // DevStatusReq
			int8_t m = snr < -32 ? -32 : snr > 31 ? 31 : snr;
			const uint8_t v[2] = {_p.battery, (uint8_t)(m & 0x3F)};
			addAnswer(0x06, v, 2);
			i = (uint8_t)(i + 1);
			break;
		}
		case 0x07:   // NewChannelReq: not in this region (no answer)
			if (left < 5) return;
			i = (uint8_t)(i + 6);
			break;
		case 0x08:   // RXTimingSetupReq
			if (left < 1) return;
			_rx1Delay = (uint8_t)(a[0] & 0x0F);
			if (_rx1Delay == 0) _rx1Delay = 1;
			addSticky(0x08, nullptr, 0);
			i = (uint8_t)(i + 2);
			break;
		case 0x09:   // TxParamSetupReq: AS923 only (no answer)
			if (left < 1) return;
			i = (uint8_t)(i + 2);
			break;
		case 0x0A:   // DlChannelReq: not in this region (no answer)
			if (left < 4) return;
			i = (uint8_t)(i + 5);
			break;
		case 0x0D: {   // DeviceTimeAns
			if (left < 5) return;
			MacEvent e;
			memset(&e, 0, sizeof e);
			e.kind = mac_ev_device_time;
			e.gpsSeconds = (uint32_t)a[0] | ((uint32_t)a[1] << 8) | ((uint32_t)a[2] << 16) | ((uint32_t)a[3] << 24);
			e.gpsFraction = a[4];
			e.ticks = _txEnd;
			push(e);
			i = (uint8_t)(i + 6);
			break;
		}
		default:   // unknown: the rest can't be parsed
			DBG_FAULT("lwmac", "unknown-cmd", cid);
			return;
		}
	}
}

// ---- frames ---------------------------------------------------------------------

bool Mac::buildFrame() {
	const uint8_t n = _region.maxPayload(_txDr);
	if (_len > n) {
		DBG_FAULT("lwmac", "too-long", _len, n);
		return false;
	}
	// FOpts: sticky answers, answers, then requests, whole commands while they fit.
	const uint8_t room = (uint8_t)((n - _len) < kMaxFOpts ? (n - _len) : kMaxFOpts);
	_foptsLen = 0;
	for (uint8_t i = 0; i < _stickyLen;) {
		const uint8_t l = upCmdLen(_sticky[i]);
		if (l == 0) break;
		if (_foptsLen + l <= room) { memcpy(_fopts + _foptsLen, _sticky + i, l); _foptsLen = (uint8_t)(_foptsLen + l); }
		i = (uint8_t)(i + l);
	}
	uint8_t keep = 0;
	for (uint8_t i = 0; i < _ansLen;) {
		const uint8_t l = upCmdLen(_ans[i]);
		if (l == 0) break;
		if (_foptsLen + l <= room) { memcpy(_fopts + _foptsLen, _ans + i, l); _foptsLen = (uint8_t)(_foptsLen + l); }
		else { memmove(_ans + keep, _ans + i, l); keep = (uint8_t)(keep + l); }   // waits for the next uplink
		i = (uint8_t)(i + l);
	}
	_ansLen = keep;
	_sentLinkCheck = _sentDeviceTime = false;
	if (_wantLinkCheck && _foptsLen + 1 <= room) { _fopts[_foptsLen++] = 0x02; _wantLinkCheck = false; _sentLinkCheck = true; }
	if (_wantDeviceTime && _foptsLen + 1 <= room) { _fopts[_foptsLen++] = 0x0D; _wantDeviceTime = false; _sentDeviceTime = true; }

	Uplink u;
	memset(&u, 0, sizeof u);
	u.confirmed = _confirmed;
	u.devAddr = _devAddr;
	u.fCtrl = (uint8_t)((_p.adr ? kFCtrlAdr : 0) | (_adrAckReq ? kFCtrlAdrAckReq : 0) | (_ackDownlink ? kFCtrlAck : 0));
	u.fCnt = _frameFCnt;
	u.fOpts = _fopts;
	u.fOptsLen = _foptsLen;
	u.hasPort = true;
	u.port = _port;
	u.payload = _data;
	u.payloadLen = _len;
	_frameLen = buildUplink(_nwkSKey, _appSKey, u, _frame, sizeof _frame);
	_ackDownlink = false;   // carried by this frame (and its retransmissions)
	return _frameLen != 0;
}

void Mac::startTx(uint32_t nowMs) {
	if (!_radio.accepting()) {
		if (nowMs - _stateMs > kTxStartTimeoutMs) {
			DBG_FAULT("lwmac", "radio-busy");
			pushSimple(mac_ev_fault, mac_fault_radio);
			_job = Job::None;
			_st = St::Idle;
		}
		return;
	}
	lora::Config c = lora::defaultConfig(0);
	if (_job == Job::Join) {
		if (_devNonce == 0xFFFF) {
			pushSimple(mac_ev_fault, mac_fault_nonce_spent);
			_job = Job::None;
			_st = St::Idle;
			return;
		}
		// The DevNonce is spent before it is sent: never reused, even if the
		// device resets in the middle of this.
		_joinNonceUsed = _devNonce++;
		if (!saveSession()) {
			--_devNonce;
			DBG_FAULT("lwmac", "save-failed");
			pushSimple(mac_ev_fault, mac_fault_store);
			_job = Job::None;
			_st = St::Idle;
			return;
		}
		++_joinTries;
		_txDr = _region.joinDataRate(_joinTries);
		if (!_region.nextChannel(_txDr, true, rnd(), &_txChannel, &_txFreq)) {
			_txDr = _region.minTxDr();
			if (!_region.nextChannel(_txDr, true, rnd(), &_txChannel, &_txFreq)) {
				pushSimple(mac_ev_fault, mac_fault_radio);
				_job = Job::None;
				_st = St::Idle;
				return;
			}
		}
		buildJoinRequest(_p.appKey, _p.joinEui, _p.devEui, _joinNonceUsed, _frame);
		_frameLen = kJoinRequestLen;
		c.powerDbm = (int8_t)(_region.eirpDbm(_region.defaultTxPower(), _txDr) - _p.antennaGainDb);
	} else {
		_txDr = _adr.dr;
		if (_tries == 0 && !buildFrame()) {
			pushSimple(mac_ev_fault, mac_fault_too_long);
			_job = Job::None;
			_st = St::Idle;
			return;
		}
		if (!_region.nextChannel(_txDr, false, rnd(), &_txChannel, &_txFreq)) {
			pushSimple(mac_ev_fault, mac_fault_radio);
			_job = Job::None;
			_st = St::Idle;
			return;
		}
		c.powerDbm = (int8_t)(_region.eirpDbm(_adr.txPower, _txDr) - _p.antennaGainDb);
	}
	c.freqHz = _txFreq;
	_region.txDataRate(_txDr, &c);
	c.cr = 1;
	c.preamble = 8;
	c.crc = true;
	c.invertIq = false;
	if (c.powerDbm > _radio.maxPowerDbm()) c.powerDbm = _radio.maxPowerDbm();
	if (c.powerDbm < 2) c.powerDbm = 2;
	if (!_radio.transmit(c, _frame, _frameLen)) {
		DBG_FAULT("lwmac", "tx-refused");
		pushSimple(mac_ev_fault, mac_fault_radio);
		_job = Job::None;
		_st = St::Idle;
		return;
	}
	++_tries;
	_txToaMs = (lora::timeOnAirUs(c, _frameLen) + 999u) / 1000u;
	DBG_EVENT("lwmac", _job == Job::Join ? "join-tx" : "tx", _txChannel, _txDr, (int32_t)_frameFCnt, _tries);
	_stateMs = nowMs;
	_st = St::TxWait;
}

void Mac::planWindows(uint32_t txEndTicks) {
	const bool joining = _job == Job::Join;
	const uint32_t d1 = joining ? kJoinAcceptDelay1Ms : (uint32_t)_rx1Delay * 1000u;
	_rxDr[0] = _region.rx1DataRate(_txDr, joining ? 0 : _rx1DrOffset);
	_rxFreq[0] = _region.rx1Frequency(_txChannel, _txFreq);
	_rxDr[1] = joining ? _region.rx2DefaultDataRate() : _rx2Dr;
	_rxFreq[1] = joining ? _region.rx2DefaultFrequency() : _rx2Freq;
	for (uint8_t w = 0; w < 2; ++w) {
		lora::Config c = lora::defaultConfig(0);
		_region.rxDataRate(_rxDr[w], &c);
		int32_t off = 0;
		rxWindow(lora::symbolUs(c.sf, c.bw), _p.minRxSymbols, _p.rxErrorMs, _p.radioWakeMs, &_rxSymbols[w], &off);
		const int32_t at = (int32_t)(d1 + 1000u * w) + off;
		_rxOpen[w] = at >= 0 ? txEndTicks + msToTicks((uint32_t)at) : txEndTicks - msToTicks((uint32_t)-at);
	}
}

bool Mac::openWindow(uint8_t w, uint32_t nowMs) {
	(void)nowMs;
	lora::Config c = lora::defaultConfig(_rxFreq[w]);
	_region.rxDataRate(_rxDr[w], &c);
	c.cr = 1;
	c.preamble = 8;
	c.crc = false;        // downlinks carry no payload CRC
	c.invertIq = true;
	return _radio.receive(c, _rxSymbols[w]);
}

bool Mac::handleJoinAccept(uint8_t* buf, uint8_t len) {
	JoinAccept ja;
	if (!parseJoinAccept(_p.appKey, buf, len, &ja)) return false;
	// 1.0.4: JoinNonce counts up; an old one is a replay.
	if (_haveJoinNonce && ja.joinNonce <= _joinNonce) {
		DBG_FAULT("lwmac", "joinnonce-old", (int32_t)ja.joinNonce);
		return false;
	}
	deriveSessionKeys(_p.appKey, ja.joinNonce, ja.netId, _joinNonceUsed, _nwkSKey, _appSKey);
	_haveJoinNonce = true;
	_joinNonce = ja.joinNonce;
	_devAddr = ja.devAddr;
	_rx1DrOffset = (uint8_t)((ja.dlSettings >> 4) & 0x07);
	lora::Config probe;
	const uint8_t rx2 = (uint8_t)(ja.dlSettings & 0x0F);
	if (_region.rxDataRate(rx2, &probe)) _rx2Dr = rx2;
	_rx1Delay = (uint8_t)(ja.rxDelay & 0x0F);
	if (_rx1Delay == 0) _rx1Delay = 1;
	if (ja.hasCfList) _region.applyCfList(ja.cfList);
	_fCntUp = 0;
	_fCntUpSaved = _p.saveEvery;
	_fCntDown = 0;
	_downSeen = false;
	_adrAckCnt = 0;
	_joined = true;
	DBG_EVENT("lwmac", "joined", (int32_t)_devAddr, _rx1DrOffset, _rx2Dr, _rx1Delay);
	if (!saveSession()) pushSimple(mac_ev_fault, mac_fault_store);
	return true;
}

bool Mac::handleData(uint8_t* buf, uint8_t len, int16_t rssi, int8_t snr) {
	Downlink d;
	if (!parseDownlinkHeader(buf, len, &d) || d.devAddr != _devAddr) return false;
	uint32_t fc = (_fCntDown & 0xFFFF0000u) | d.fCnt16;
	if (_downSeen && fc <= _fCntDown) fc += 0x10000u;
	if (!openDownlink(_nwkSKey, _appSKey, d, fc, buf, len)) {
		DBG_FAULT("lwmac", "mic", (int32_t)fc);
		return false;
	}
	_fCntDown = fc;
	_downSeen = true;
	_gotDownlink = true;
	_adrAckCnt = 0;
	_adrAckReq = false;
	_stickyLen = 0;                                        // the network has heard them
	if ((d.fCtrl & kFCtrlAck) && _confirmed) _acked = true;
	if (d.mtype == mtype_confirmed_down) _ackDownlink = true;
	if (d.fCtrl & kFCtrlFPending) _fPendingSeen = true;
	if (d.hasPort && d.port == 0) handleCommands(buf + d.payloadOff, d.payloadLen, snr);
	else if (d.fOptsLen) handleCommands(buf + d.fOptsOff, d.fOptsLen, snr);
	if (d.hasPort && d.port != 0) {
		if (_downHeld) {
			++_dropped;   // the last one hasn't been taken
			DBG_FAULT("lwmac", "downlink-dropped");
		} else {
			memcpy(_down, buf + d.payloadOff, d.payloadLen);
			_downHeld = true;
			MacEvent e;
			memset(&e, 0, sizeof e);
			e.kind = mac_ev_downlink;
			e.port = d.port;
			e.len = d.payloadLen;
			e.rssiDbm = rssi;
			e.snrDb = snr;
			e.fPending = (d.fCtrl & kFCtrlFPending) != 0;
			push(e);
		}
	}
	DBG_EVENT("lwmac", "down", (int32_t)fc, d.hasPort ? d.port : -1, d.payloadLen);
	return true;
}

bool Mac::handleRx(const uint8_t* buf, uint8_t len, int16_t rssi, int8_t snr) {
	memcpy(_rxBuf, buf, len);
	if (_job == Job::Join) return handleJoinAccept(_rxBuf, len);
	if (_job == Job::Data) return handleData(_rxBuf, len, rssi, snr);
	return false;
}

void Mac::finishCycle(uint32_t nowMs) {
	// Duty cycle (DutyCycleReq): this transmission's time on air times 2^n - 1
	// off, from the end of the transmission (as a band's time-off in LoRaMac-node).
	if (_maxDutyCycle) _nextTxMs = _txEndMs + _txToaMs * ((1u << _maxDutyCycle) - 1u);
	_dutyLimit = _maxDutyCycle != 0;

	if (_job == Job::Join) {
		if (_joined) {
			_job = Job::None;
			_st = St::Idle;
			pushSimple(mac_ev_joined);
			return;
		}
		if (_joinMax && _joinTries >= _joinMax) {
			_job = Job::None;
			_st = St::Idle;
			pushSimple(mac_ev_join_failed);
			return;
		}
		// RP002's join backoff: 1% of the time in the first hour since the
		// join began, 0.1% to hour 11, 0.01% after (LoRaMac-node's BACKOFF_DC_*).
		const uint32_t since = nowMs - _joinStartMs;
		const uint32_t factor = since < 3600000u ? 100u : since < 39600000u ? 1000u : 10000u;
		_waitUntilMs = _txEndMs + _txToaMs * (factor - 1u) + rnd() % 1000u;
		if ((int32_t)(_waitUntilMs - nowMs) < 0) _waitUntilMs = nowMs;
		_st = St::Wait;
		return;
	}
	if (_job == Job::Data) {
		const bool again = _confirmed ? !_acked : !_gotDownlink;
		if (again && _tries < _triesMax) {
			// RETRANSMIT_TIMEOUT: 2 s +/- 1 s after the receive windows.
			_waitUntilMs = nowMs + 1000u + rnd() % 2001u;
			_st = St::Wait;
			return;
		}
		MacEvent e;
		memset(&e, 0, sizeof e);
		e.kind = mac_ev_tx_done;
		e.ack = _acked;
		e.fPending = _fPendingSeen;
		push(e);
		_job = Job::None;
		_st = St::Idle;
		return;
	}
	_job = Job::None;
	_st = St::Idle;
}

// ---- main ---------------------------------------------------------------------------

void Mac::main(uint32_t nowMs) {
	_radio.main(nowMs);
	const uint32_t now = _radio.now(nowMs);

	lora::Event ev;
	bool have = _radio.takeEvent(&ev, _rxBuf, sizeof _rxBuf);
	if (have && ev.kind == lora::ev_fault) {
		DBG_FAULT("lwmac", "radio-fault");
		pushSimple(mac_ev_fault, mac_fault_radio);
		if (_st != St::Idle && _st != St::Wait) {
			_stateMs = nowMs;
			if (_job == Job::Join) { _waitUntilMs = nowMs + 1000u; _st = St::Wait; }
			else { _job = Job::None; _st = St::Idle; }
		}
		return;
	}

	switch (_st) {
	case St::Idle:
		_stateMs = nowMs;
		break;
	case St::Wait:
		if ((int32_t)(nowMs - _waitUntilMs) >= 0 && (!_dutyLimit || (int32_t)(nowMs - _nextTxMs) >= 0)) {
			_stateMs = nowMs;
			_st = St::TxStart;
			startTx(nowMs);
		}
		break;
	case St::TxStart:
		startTx(nowMs);
		break;
	case St::TxWait:
		if (have && ev.kind == lora::ev_tx_done) {
			_txEnd = ev.ticks;
			_txEndMs = nowMs;
			planWindows(_txEnd);
			_stateMs = nowMs;
			_st = St::Rx1Wait;
		} else if (nowMs - _stateMs > _txToaMs + kRadioGuardMs) {
			pushSimple(mac_ev_fault, mac_fault_radio);
			_job = Job::None;
			_st = St::Idle;
		}
		break;
	case St::Rx1Wait:
	case St::Rx2Wait: {
		const uint8_t w = _st == St::Rx1Wait ? 0 : 1;
		if (after(_rxOpen[w], now)) {
			if (openWindow(w, nowMs)) {
				_stateMs = nowMs;
				_st = w == 0 ? St::Rx1 : St::Rx2;
			} else if (nowMs - _stateMs > kRadioGuardMs) {
				pushSimple(mac_ev_fault, mac_fault_radio);
				finishCycle(nowMs);
			}
		}
		break;
	}
	case St::Rx1:
	case St::Rx2: {
		const bool second = _st == St::Rx2;
		bool done = false, next = false;
		if (have) {
			if (ev.kind == lora::ev_rx_done) {
				if (handleRx(_rxBuf, ev.len, ev.rssiDbm, ev.snrDb)) done = true;
				else next = true;
			} else if (ev.kind == lora::ev_rx_timeout || ev.kind == lora::ev_crc_error) {
				next = true;
			}
		} else if (nowMs - _stateMs > kRadioGuardMs) {
			_radio.standby();
			next = true;
		}
		if (done || (next && second)) {
			finishCycle(nowMs);
		} else if (next) {
			_stateMs = nowMs;
			_st = St::Rx2Wait;
		}
		break;
	}
	}
}

uint32_t Mac::sleepHintMs(uint32_t nowMs) const {
	switch (_st) {
	case St::Idle:
		return 100;
	case St::Wait: {
		uint32_t t = _waitUntilMs;
		if (_dutyLimit && (int32_t)(_nextTxMs - t) > 0) t = _nextTxMs;
		const int32_t left = (int32_t)(t - nowMs);
		return left <= 0 ? 0 : left > 100 ? 100 : (uint32_t)left;
	}
	case St::Rx1Wait:
	case St::Rx2Wait: {
		const int32_t left = (int32_t)(_rxOpen[_st == St::Rx1Wait ? 0 : 1] - _radio.now(nowMs));
		if (left <= 0) return 0;
		const uint32_t ms = (uint32_t)((uint64_t)left * 1000u / _radio.ticksPerSecond());
		return ms > 100 ? 100 : ms;   // rounded down: wakes at or before the window
	}
	default:
		return 1;   // the radio is working: its poll, or a DIO interrupt
	}
}

} // namespace lorawan
