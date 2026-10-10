/*
 * ublox_gps.tpp
 *
 *  ublox_gps's member definitions. ublox_gps.h includes this file at
 *  its end, since templates have to be visible wherever they are
 *  used: include ublox_gps.h, not this file.
 */

#ifndef UBLOX_GPS_TPP_
#define UBLOX_GPS_TPP_

#include "../inc/ublox_gps.h"

template <typename TTransport>
template <typename... TArgs>
ublox_gps<TTransport>::ublox_gps(const ublox_gps_param_t& param, TArgs&&... transportArgs)
	: base(ublox_init_state, ublox_error_state, 0, std::forward<TArgs>(transportArgs)...),
	  _param(param), _head(0), _tail(0), _overflows(0), _wakePending(false), _sinceWake(0),
	  _txLen(0), _cfgStep(0), _cfgTries(0), _waitCls(0), _waitId(0), _ack(Ack::Waiting),
	  _configStatus(ublox_config_status_t::NotDone), _cfgResends(0),
	  _lastGoodMs(0), _haveData(false), _newData(false) {
	memset(_ring, 0, sizeof(_ring));
	memset(_tx, 0, sizeof(_tx));
	if (_param.measRateMs < 25) _param.measRateMs = 25;   // 40 Hz: more than any of them does
}

// ---- interrupt side ----

template <typename TTransport>
void ublox_gps<TTransport>::onByteReceived(uint8_t byte) {
	const uint32_t head = _head.load(std::memory_order_relaxed);
	const uint32_t tail = _tail.load(std::memory_order_acquire);
	if (head - tail >= kRxRingSize) {
		// Only this interrupt writes the counter: a load and a store,
		// no read-modify-write, so it needs nothing a Cortex-M0 lacks.
		_overflows.store(_overflows.load(std::memory_order_relaxed) + 1, std::memory_order_release);
		return;
	}
	_ring[head & (kRxRingSize - 1)] = byte;
	_head.store(head + 1, std::memory_order_release);

	if (byte == '\n' || ++_sinceWake >= 64) {
		_sinceWake = 0;
		_wakePending.store(true, std::memory_order_release);
		wake();
	}
}

// ---- main() side ----

template <typename TTransport>
void ublox_gps<TTransport>::drain(uint32_t nowMs) {
	_wakePending.store(false, std::memory_order_release);
	uint32_t       tail = _tail.load(std::memory_order_relaxed);
	const uint32_t head = _head.load(std::memory_order_acquire);
	while (tail != head) {
		const uint8_t b = _ring[tail & (kRxRingSize - 1)];
		++tail;
		_tail.store(tail, std::memory_order_release);
		handleByte(b, nowMs);
	}
}

template <typename TTransport>
void ublox_gps<TTransport>::handleByte(uint8_t b, uint32_t nowMs) {
	switch (_ubx.feed(b)) {
	case ubx::Parser::Result::Consumed:
	case ubx::Parser::Result::Bad:
		return;
	case ubx::Parser::Result::Frame: {
		bool acked;
		if (_state == ublox_cfg_wait_state && _ubx.isAckFor(_waitCls, _waitId, &acked)) {
			_ack = acked ? Ack::Acked : Ack::Refused;
		}
		return;
	}
	case ubx::Parser::Result::NotMine:
		break;
	}

	const NmeaParser::Sentence s = _nmea.feed(b);
	if (s == NmeaParser::Sentence::None || s == NmeaParser::Sentence::Bad) return;
	_lastGoodMs = nowMs;   // any good sentence shows the receiver is there
	if (s == NmeaParser::Sentence::GGA || s == NmeaParser::Sentence::RMC) {
		_haveData = true;
		_newData = true;
	}
}

template <typename TTransport>
bool ublox_gps<TTransport>::nextConfigMessage() {
	const uint8_t n = (uint8_t)ubx::Nmea::Count;
	if (_param.config == ublox_config_t::Legacy) {
		if (_cfgStep < n) {
			const ubx::Nmea s = (ubx::Nmea)_cfgStep;
			const uint8_t rate = (_param.sentences & ublox_sentence_bit(s)) ? 1 : 0;
			_txLen = ubx::cfgMsg(_tx, s, rate);
			_waitCls = ubx::kClassCfg;
			_waitId = ubx::kIdCfgMsg;
			return true;
		}
		if (_cfgStep == n) {
			_txLen = ubx::cfgRate(_tx, _param.measRateMs, 1, 1);   // one solution a measurement, GPS time
			_waitCls = ubx::kClassCfg;
			_waitId = ubx::kIdCfgRate;
			return true;
		}
		return false;
	}
	if (_param.config == ublox_config_t::ValSet && _cfgStep == 0) {
		ubx::ValSet v(ubx::kLayerRam);
		for (uint8_t i = 0; i < n; ++i) {
			const ubx::Nmea s = (ubx::Nmea)i;
			v.addU1(ubx::nmeaUart1Key(s), (_param.sentences & ublox_sentence_bit(s)) ? 1 : 0);
		}
		v.addU2(ubx::kKeyRateMeas, _param.measRateMs);
		v.addU2(ubx::kKeyRateNav, 1);
		_txLen = v.build(_tx);
		_waitCls = ubx::kClassCfg;
		_waitId = ubx::kIdCfgValSet;
		return true;
	}
	return false;
}

template <typename TTransport>
void ublox_gps<TTransport>::main(uint32_t nowMs) {
	drain(nowMs);

	switch (_state) {
	case ublox_init_state:
		// From here on the transport calls onByteReceived(). This also
		// starts its receiver if it is not already running.
		this->setRxSink(*this);
		_cfgStep = 0;
		_cfgTries = 0;
		_lastGoodMs = nowMs;
		if (_param.config == ublox_config_t::None) {
			enter(ublox_listening_state, nowMs);
		} else {
			_configStatus = ublox_config_status_t::NotDone;
			enter(ublox_cfg_send_state, nowMs);
		}
		break;

	case ublox_cfg_send_state:
		// Built again on every pass until write() takes it. Writing over
		// _tx while the UART may still be sending it is harmless here:
		// the bytes are the same, since the step has not moved on. It
		// moves on only after an ACK, which the receiver sends after the
		// whole message has arrived.
		if (!nextConfigMessage()) {
			_configStatus = ublox_config_status_t::Done;
			enter(ublox_listening_state, nowMs);
			break;
		}
		_ack = Ack::Waiting;
		if (this->write(_tx, _txLen)) {
			enter(ublox_cfg_wait_state, nowMs);
		} else if (elapsed(nowMs, ublox_write_timeout_ms)) {
			// The UART will not take it: carry on unconfigured.
			_configStatus = ublox_config_status_t::NoAnswer;
			enter(ublox_listening_state, nowMs);
		} else {
			sleep(1);
		}
		break;

	case ublox_cfg_wait_state:
		if (_ack == Ack::Acked) {
			++_cfgStep;
			_cfgTries = 0;
			enter(ublox_cfg_send_state, nowMs);
		} else if (_ack == Ack::Refused) {
			_configStatus = ublox_config_status_t::Rejected;
			enter(ublox_listening_state, nowMs);
		} else if (elapsed(nowMs, ublox_ack_timeout_ms)) {
			if (++_cfgTries < ublox_cfg_tries) {
				++_cfgResends;
				enter(ublox_cfg_send_state, nowMs);   // the same message again
			} else {
				_configStatus = ublox_config_status_t::NoAnswer;
				enter(ublox_listening_state, nowMs);
			}
		} else {
			// An ACK does not end in a newline, so it may not wake an
			// OS thread by itself: look again soon.
			sleep(5);
		}
		break;

	case ublox_listening_state:
		if (nowMs - _lastGoodMs >= _param.silenceMs) {
			fail(nowMs);
		} else {
			const uint32_t left = _param.silenceMs - (nowMs - _lastGoodMs);
			sleep(left < ublox_max_sleep_ms ? left : ublox_max_sleep_ms);
		}
		break;

	case ublox_error_state:
		if (elapsed(nowMs, ublox_retry_ms)) {
			enter(ublox_init_state, nowMs);   // attach again and configure again: it may have been power cycled
		} else {
			sleepRemaining(nowMs, ublox_retry_ms);
		}
		break;

	default:
		enter(ublox_init_state, nowMs);
		break;
	}
}

template <typename TTransport>
void ublox_gps<TTransport>::onFail() {
	_nmea.invalidate();
	_haveData = false;
	_newData = false;
}

template <typename TTransport>
bool ublox_gps<TTransport>::getData(GnssData* out) {
	*out = _nmea.data();
	_newData = false;
	return _haveData;
}

template <typename TTransport>
bool ublox_gps<TTransport>::position(int32_t* latE7, int32_t* lonE7) const {
	const GnssData& d = _nmea.data();
	if (!_haveData || !d.positionValid) return false;
	*latE7 = d.latitudeE7;
	*lonE7 = d.longitudeE7;
	return true;
}

template <typename TTransport>
typename ublox_gps<TTransport>::Stats ublox_gps<TTransport>::stats() const {
	Stats s;
	s.rxOverflows = _overflows.load(std::memory_order_acquire);
	s.ubxFrames = _ubx.frames();
	s.ubxErrors = _ubx.errors();
	s.cfgResends = _cfgResends;
	return s;
}

#endif /* UBLOX_GPS_TPP_ */
