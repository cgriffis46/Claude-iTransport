/*
 * MeshNode.cpp
 *
 *  See MeshNode.h. Timing rules from MeshCore's src/Dispatcher.cpp
 *  (checkSend, updateTxBudget) and src/Mesh.cpp (getCADFailRetryDelay,
 *  sendFlood, onRecvPacket).
 */

#include "MeshNode.h"
#include <string.h>
#include "DebugLog.h"

namespace meshcore {

uint16_t preambleForSf(uint8_t sf) { return sf <= 8 ? 32 : 16; }

NodeParam defaultNodeParam() {
	NodeParam p;
	p.radio = lora::defaultConfig(910525000u);
	p.radio.sf = 7;
	p.radio.bw = lora::Bw::Bw62_5k;
	p.radio.cr = 1;                 // 4/5 ("CR5")
	p.radio.preamble = preambleForSf(7);
	p.radio.crc = true;
	p.radio.invertIq = false;
	p.radio.powerDbm = 17;
	p.pathHashSize = 1;
	p.dutyPercent = 50;
	p.lbtMaxMs = 4000;
	p.randomSeed = 0x9E3779B9u;
	return p;
}

Node::Node(lora::iLoRaRadio& radio, const LocalIdentity& id, const NodeParam& param)
	: _radio(radio), _id(id), _p(param), _rng(param.randomSeed ? param.randomSeed : 1u) {
	if (_p.pathHashSize < 1 || _p.pathHashSize > 3) _p.pathHashSize = 1;
	if (_p.dutyPercent == 0 || _p.dutyPercent > 100) _p.dutyPercent = 50;
	memset(_seen, 0, sizeof _seen);
}

uint32_t Node::rnd() {   // xorshift32
	_rng ^= _rng << 13;
	_rng ^= _rng >> 17;
	_rng ^= _rng << 5;
	return _rng;
}

void Node::begin(uint32_t nowMs) {
	_budgetMs = (uint32_t)((uint64_t)kBudgetWindowMs * _p.dutyPercent / 100u);
	_budgetAtMs = nowMs;
	_nextTryMs = nowMs;
	_started = true;
	listen();
}

int8_t Node::addChannel(const uint8_t* key, uint8_t len) {
	if (_chCount >= kChannels || !_ch[_chCount].set(key, len)) return -1;
	return (int8_t)_chCount++;
}

// ---- the duplicate table ----

bool Node::seen(const uint8_t hash[kHashSize]) const {
	for (uint8_t i = 0; i < _seenCount; ++i)
		if (memcmp(_seen[i], hash, kHashSize) == 0) return true;
	return false;
}

void Node::markSeen(const uint8_t hash[kHashSize]) {
	memcpy(_seen[_seenNext], hash, kHashSize);
	_seenNext = (uint8_t)((_seenNext + 1) % kSeen);
	if (_seenCount < kSeen) ++_seenCount;
}

// ---- sending ----

bool Node::sendPacket(const Packet& pkt) {
	if (_qCount >= kQueue) { ++_st.queueFull; return false; }
	Slot& s = _q[(uint8_t)((_qHead + _qCount) % kQueue)];
	s.len = pkt.encode(s.raw, sizeof s.raw);
	if (s.len == 0) return false;
	s.type = pkt.payloadType();
	uint8_t h[kHashSize];
	pkt.hash(h);
	markSeen(h);   // a repeater's copy of it, coming back, is a duplicate
	++_qCount;
	return true;
}

bool Node::sendAdvert(uint32_t timestamp, const AdvertData& data, bool flood) {
	Packet p;
	if (!buildAdvert(_id, timestamp, data, flood, p)) return false;
	if (flood) p.pathLen = (uint8_t)((_p.pathHashSize - 1) << 6);
	return sendPacket(p);
}

bool Node::sendGroupText(uint8_t channel, uint32_t timestamp, const char* sender, const char* text) {
	if (channel >= _chCount) return false;
	Packet p;
	if (!buildGroupText(_ch[channel], timestamp, sender, text, p)) return false;
	p.pathLen = (uint8_t)((_p.pathHashSize - 1) << 6);
	return sendPacket(p);
}

bool Node::sendGroupData(uint8_t channel, uint16_t dataType, const uint8_t* data, uint8_t len) {
	if (channel >= _chCount) return false;
	Packet p;
	if (!buildGroupData(_ch[channel], dataType, data, len, p)) return false;
	p.pathLen = (uint8_t)((_p.pathHashSize - 1) << 6);
	return sendPacket(p);
}

// Dispatcher::updateTxBudget: the budget refills at the duty cycle, up to
// the window's worth.
void Node::refill(uint32_t nowMs) {
	const uint32_t elapsed = nowMs - _budgetAtMs;
	const uint32_t add = (uint32_t)((uint64_t)elapsed * _p.dutyPercent / 100u);
	if (add == 0) return;
	const uint32_t max = (uint32_t)((uint64_t)kBudgetWindowMs * _p.dutyPercent / 100u);
	_budgetMs = (_budgetMs + add > max) ? max : _budgetMs + add;
	_budgetAtMs = nowMs;
}

void Node::trySend(uint32_t nowMs) {
	if (_qCount == 0 || _sending) return;
	refill(nowMs);
	// Enough budget for half the longest packet (MIN_TX_BUDGET_AIRTIME_DIV).
	const uint32_t longest = lora::timeOnAirUs(_p.radio, kMaxPacket) / 1000u;
	if (_budgetMs < longest / 2) {
		const uint32_t needed = longest / 2 - _budgetMs;
		_nextTryMs = nowMs + (uint32_t)((uint64_t)needed * 100u / _p.dutyPercent);
		++_st.budgetWaits;
		DBG_EVENT("mesh", "budget-wait", (int32_t)needed);
		return;
	}
	if ((int32_t)(nowMs - _nextTryMs) < 0) return;
	if (_radio.channelBusy()) {
		if (!_lbtWaiting) { _lbtWaiting = true; _lbtStartMs = nowMs; }
		if (nowMs - _lbtStartMs <= _p.lbtMaxMs) {
			_nextTryMs = nowMs + 120u * (1u + rnd() % 4u);   // Mesh::getCADFailRetryDelay
			++_st.lbtWaits;
			return;
		}
		++_st.lbtForced;   // busy too long: the radio may be confused; send anyway
		DBG_FAULT("mesh", "lbt-forced");
	}
	_lbtWaiting = false;
	if (!_radio.accepting()) return;
	const Slot& s = _q[_qHead];
	if (!_radio.transmit(_p.radio, s.raw, s.len)) return;
	_listening = false;   // the transmission ends RX continuous
	_sending = true;
	_sendingType = s.type;
	_sendStartMs = nowMs;
	_sendAirMs = (lora::timeOnAirUs(_p.radio, s.len) + 999u) / 1000u;
	_qHead = (uint8_t)((_qHead + 1) % kQueue);
	--_qCount;
	DBG_EVENT("mesh", "tx", (int32_t)_sendingType, s.len);
}

void Node::listen() {
	if (_started && !_sending && !_listening && _radio.accepting() && _radio.receive(_p.radio, 0)) _listening = true;
}

// ---- receiving ----

void Node::handleRx(const uint8_t* raw, uint8_t len, int16_t rssi, int8_t snr, uint32_t ticks) {
	++_st.received;
	Packet p;
	if (!p.decode(raw, len) || p.payloadVersion() != 0) { ++_st.malformed; return; }
	uint8_t h[kHashSize];
	p.hash(h);
	if (seen(h)) { ++_st.duplicates; return; }
	markSeen(h);
	NodeEvent e = NodeEvent();
	e.rssiDbm = rssi;
	e.snrDb = snr;
	e.ticks = ticks;
	e.hops = p.hopCount();
	switch (p.payloadType()) {
	case payload_advert: {
		Advert a;
		if (!parseAdvert(p, &a)) { ++_st.badAdverts; DBG_FAULT("mesh", "bad-advert"); return; }
		if (memcmp(a.pubKey, _id.pubKey(), kPubKeySize) == 0) return;   // our own, back
		e.kind = node_ev_advert;
		memcpy(e.pubKey, a.pubKey, kPubKeySize);
		e.timestamp = a.timestamp;
		e.advert = a.data;
		e.advertDataValid = a.dataValid;
		push(e, nullptr, 0);
		break;
	}
	case payload_grp_txt:
	case payload_grp_data: {
		GroupMessage m;
		if (!openGroup(p, _ch, _chCount, &m)) { ++_st.unopened; return; }   // not our channel
		e.kind = m.isText ? node_ev_group_text : node_ev_group_data;
		e.channel = m.channel;
		e.timestamp = m.timestamp;
		e.dataType = m.dataType;
		push(e, m.data, m.len);
		break;
	}
	default:
		++_st.ignored;   // direct messages, paths, ACKs...: not handled yet
		break;
	}
}

void Node::push(const NodeEvent& e, const uint8_t* data, uint8_t len) {
	if (_evCount == kEvents) { ++_st.eventsDropped; DBG_FAULT("mesh", "event-dropped", (int32_t)e.kind); return; }
	Ev& v = _ev[(uint8_t)((_evHead + _evCount) % kEvents)];
	v.e = e;
	v.e.len = len;
	if (len) memcpy(v.data, data, len);
	++_evCount;
}

bool Node::takeEvent(NodeEvent* e, uint8_t* buf, uint8_t cap) {
	if (_evCount == 0) return false;
	const Ev& v = _ev[_evHead];
	*e = v.e;
	if (buf && cap) {
		const uint8_t n = v.e.len < cap ? v.e.len : (uint8_t)(cap - 1);
		memcpy(buf, v.data, n);
		if (v.e.kind == node_ev_group_text && n < cap) buf[n] = 0;   // text: a C string
		e->len = n;
	}
	_evHead = (uint8_t)((_evHead + 1) % kEvents);
	--_evCount;
	return true;
}

// ---- main ----

void Node::main(uint32_t nowMs) {
	_radio.main(nowMs);
	lora::Event ev;
	while (_radio.takeEvent(&ev, _rx, sizeof _rx)) {
		switch (ev.kind) {
		case lora::ev_rx_done:
			handleRx(_rx, ev.len, ev.rssiDbm, ev.snrDb, ev.ticks);
			break;
		case lora::ev_tx_done: {
			_sending = false;
			++_st.sent;
			// Dispatcher: the budget pays the airtime actually used.
			refill(nowMs);
			const uint32_t used = nowMs - _sendStartMs > _sendAirMs ? _sendAirMs : nowMs - _sendStartMs;
			_budgetMs = _budgetMs > used ? _budgetMs - used : 0;
			NodeEvent e = NodeEvent();
			e.kind = node_ev_sent;
			e.type = _sendingType;
			e.ticks = ev.ticks;
			push(e, nullptr, 0);
			break;
		}
		case lora::ev_fault: {
			_sending = false;
			_listening = false;   // the radio restarts; listen again when it is back
			NodeEvent e = NodeEvent();
			e.kind = node_ev_fault;
			push(e, nullptr, 0);
			break;
		}
		default:   // CRC errors (counted by the radio); no timeouts in RX continuous
			break;
		}
	}
	if (!_started) return;
	trySend(nowMs);
	listen();
}

uint32_t Node::sleepHintMs(uint32_t nowMs) const {
	// The radio needs main() back to back while it works (rfm95's sleep()
	// does nothing), and the node listens again as soon as it can.
	if (_sending || !_listening || _radio.busy()) return 1;
	if (_qCount) {
		const int32_t left = (int32_t)(_nextTryMs - nowMs);
		return left <= 0 ? 1 : left > 100 ? 100 : (uint32_t)left;
	}
	return 100;
}

} // namespace meshcore
