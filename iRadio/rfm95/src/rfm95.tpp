/*
 * rfm95.tpp
 *
 *  Member definitions of rfm95<TTransport>. Included at the end of
 *  inc/rfm95.h; not compiled on its own.
 */

#include "../inc/rfm95.h"

namespace rfm95_detail {
// An SPITransport is told the SX1276 sets bit 7 to write. Any other
// transport (a test's, say) takes plain register numbers already.
template <typename T> inline void addressBit(T& t, std::true_type) {
	t.setAddressBit(SPITransport::AddressBit::WriteHigh);
}
template <typename T> inline void addressBit(T&, std::false_type) {}

// RegOcp for a current limit: Ocp on | trim, as arduino-LoRa computes it.
inline uint8_t ocp(uint8_t mA) {
	uint8_t trim = 27;
	if (mA <= 120) trim = (uint8_t)((mA - 45) / 5);
	else if (mA <= 240) trim = (uint8_t)((mA + 30) / 10);
	return (uint8_t)(sx1276::kOcpOn | (trim & 0x1F));
}
} // namespace rfm95_detail

template <typename TTransport>
template <typename... TArgs>
rfm95<TTransport>::rfm95(const rfm95_param_t& p, TArgs&&... transportArgs)
	: Base(rfm95_init, rfm95_error, kBusTimeoutMs, std::forward<TArgs>(transportArgs)...),
	  _param(p), _cfg(lora::defaultConfig(915000000u)) {
	rfm95_detail::addressBit(static_cast<TTransport&>(*this), std::is_base_of<SPITransport, TTransport>());
	if (_param.pollMs == 0) _param.pollMs = 1;
	if (_param.maxPowerDbm > 20) _param.maxPowerDbm = 20;
	memset(_tx, 0, sizeof _tx);
	memset(_rxBuf, 0, sizeof _rxBuf);
	memset(_regs, 0, sizeof _regs);
	this->setDebugTag("rfm95");
}

// ---- requests ----

template <typename TTransport>
bool rfm95<TTransport>::transmit(const lora::Config& cfg, const uint8_t* data, uint8_t len) {
	if (!accepting() || !lora::valid(cfg) || len == 0 || data == nullptr) return false;
	_cfg = cfg;
	memcpy(_tx, data, len);
	_txLen = len;
	_req = Req::Tx;
	return true;
}

template <typename TTransport>
bool rfm95<TTransport>::receive(const lora::Config& cfg, uint16_t timeoutSymbols) {
	if (!accepting() || !lora::valid(cfg) || timeoutSymbols > 1023) return false;
	_cfg = cfg;
	_rxSymbols = timeoutSymbols;
	_req = Req::Rx;
	return true;
}

template <typename TTransport>
bool rfm95<TTransport>::standby() {
	if (!receivingContinuous()) return false;
	_req = Req::Standby;
	return true;
}

template <typename TTransport>
bool rfm95<TTransport>::powerDown() {
	if (!ready()) return false;
	_req = Req::Sleep;
	return true;
}

template <typename TTransport>
bool rfm95<TTransport>::takeEvent(rfm95_event_t* e, uint8_t* buf, uint8_t cap) {
	if (_evCount == 0) return false;
	const Slot& s = _ev[_evHead];
	*e = s.e;
	if (buf && s.e.kind == rfm95_ev_rx_done) memcpy(buf, s.data, s.e.len < cap ? s.e.len : cap);
	_evHead = (uint8_t)((_evHead + 1) % kEvents);
	--_evCount;
	return true;
}

template <typename TTransport>
void rfm95<TTransport>::push(rfm95_event_kind_t k, uint32_t ticks, int16_t rssi, int8_t snr, uint8_t len) {
	if (_evCount == kEvents) {
		++_st.eventsDropped;
		DBG_FAULT("rfm95", "event-dropped", (int32_t)k);
		return;
	}
	Slot& s = _ev[(_evHead + _evCount) % kEvents];
	s.e.kind = k;
	s.e.ticks = ticks;
	s.e.rssiDbm = rssi;
	s.e.snrDb = snr;
	s.e.len = len;
	if (k == rfm95_ev_rx_done) memcpy(s.data, _rxBuf, len);
	++_evCount;
}

// ---- transfers ----

template <typename TTransport>
void rfm95<TTransport>::op3(uint8_t reg, uint8_t len, uint8_t v0, uint8_t v1, uint8_t v2) {
	if (_opCount >= sizeof(_ops) / sizeof(_ops[0])) { DBG_FAULT("rfm95", "op-overflow"); return; }   // 26 at most (a 255 byte TX at 500 kHz, measured)
	Op& o = _ops[_opCount++];
	o.reg = reg; o.len = len; o.ptr = nullptr;
	o.v[0] = v0; o.v[1] = v1; o.v[2] = v2;
}

// A write from a buffer that stays put (the TX payload): FIFO data.
template <typename TTransport>
void rfm95<TTransport>::opData(uint8_t reg, const uint8_t* p, uint8_t len) {
	if (_opCount >= sizeof(_ops) / sizeof(_ops[0])) { DBG_FAULT("rfm95", "op-overflow"); return; }   // 26 at most (a 255 byte TX at 500 kHz, measured)
	Op& o = _ops[_opCount++];
	o.reg = reg; o.len = len; o.ptr = p;
}

template <typename TTransport>
void rfm95<TTransport>::runOps(rfm95_state_t after, uint32_t nowMs) {
	_opPos = 0;
	_afterOps = after;
	this->enter(_opCount ? rfm95_write_ops : after, nowMs);
}

// Everything that depends on the request's lora::Config. Every value is
// built whole; none is read back and modified.
template <typename TTransport>
void rfm95<TTransport>::queueModem(const lora::Config& c, bool forTx) {
	using namespace sx1276;
	const uint32_t f = lora::frf(c.freqHz);
	op(kRegOpMode, kOpLoRa | kOpStandby);
	op3(kRegFrfMsb, 3, (uint8_t)(f >> 16), (uint8_t)(f >> 8), (uint8_t)f);
	op(kRegModemConfig1, (uint8_t)(((uint8_t)c.bw << 4) | (c.cr << 1)));   // explicit header
	const uint16_t symb = forTx ? 0 : (_rxContinuous ? 0x3FF : _rxSymbols);
	op(kRegModemConfig2, (uint8_t)((c.sf << 4) | (c.crc ? kCrcOn : 0) | ((symb >> 8) & 0x03)));
	op(kRegModemConfig3, (uint8_t)((lora::lowDataRateOptimize(c.sf, c.bw) ? kLdro : 0) | kAgcAuto));
	op3(kRegPreambleMsb, 2, (uint8_t)(c.preamble >> 8), (uint8_t)c.preamble, 0);
	if (!forTx) op(kRegSymbTimeoutLsb, (uint8_t)symb);
	if (forTx) op(kRegInvertIq, c.invertIq ? kInvertIqTx : kInvertIqNormal);
	else op(kRegInvertIq, c.invertIq ? kInvertIqRx : kInvertIqNormal);
	op(kRegInvertIq2, c.invertIq ? kInvertIq2Inverted : kInvertIq2Normal);
	// Errata 2.1 (Semtech's code): sensitivity at 500 kHz.
	if (c.bw == lora::Bw::Bw500k) {
		op(kRegHighBwOptimize1, kHighBw500Opt1);
		if (c.freqHz > 525000000u) op(kRegHighBwOptimize2, kHighBw500Opt2Hf);
	} else {
		op(kRegHighBwOptimize1, kHighBwOtherOpt1);
	}
}

template <typename TTransport>
void rfm95<TTransport>::startRequest(uint32_t nowMs) {
	using namespace sx1276;
	opsClear();
	_deadlineMs = 0;
	switch (_req) {
	case Req::Tx: {
		_rxContinuous = false;
		queueModem(_cfg, true);
		// Power on PA_BOOST: +2..+17 dBm as PaConfig = p - 2 with the normal
		// PA DAC; +18..+20 with the high power DAC, PaConfig = p - 5.
		int8_t p = _cfg.powerDbm;
		if (p > _param.maxPowerDbm) p = _param.maxPowerDbm;
		if (p < 2) p = 2;
		if (p > 17) {
			op(kRegPaDac, kPaDac20dBm);
			op(kRegOcp, rfm95_detail::ocp(140));
			op(kRegPaConfig, (uint8_t)(kPaBoost | (uint8_t)(p - 5)));
		} else {
			op(kRegPaDac, kPaDacNormal);
			op(kRegOcp, rfm95_detail::ocp(100));
			op(kRegPaConfig, (uint8_t)(kPaBoost | (uint8_t)(p - 2)));
		}
		op(kRegPayloadLength, _txLen);
		op(kRegFifoAddrPtr, 0x00);   // the TX base
		// off is 16 bits: at 255 bytes a uint8_t would wrap from 224 to 0.
		for (uint16_t off = 0; off < _txLen; off = (uint16_t)(off + ISensorTransport::kMaxWriteLen)) {
			const uint8_t n = (uint8_t)((_txLen - off) < ISensorTransport::kMaxWriteLen ? (_txLen - off) : ISensorTransport::kMaxWriteLen);
			opData(kRegFifo, _tx + off, n);
		}
		op(kRegDioMapping1, kDio0TxDone);
		op(kRegIrqFlags, kIrqAll);
		op(kRegOpMode, kOpLoRa | kOpTx);
		_deadlineMs = lora::timeOnAirUs(_cfg, _txLen) / 1000u + kTxMarginMs;
		DBG_EVENT("rfm95", "tx", (int32_t)(_cfg.freqHz / 1000u), _cfg.sf, (int32_t)_cfg.bw, _txLen, p);
		_req = Req::None;
		_irqPending = false;
		runOps(rfm95_tx_wait, nowMs);
		break;
	}
	case Req::Rx:
		_rxContinuous = _rxSymbols == 0;
		queueModem(_cfg, false);
		op(kRegMaxPayloadLength, 0xFF);
		op(kRegFifoAddrPtr, 0x00);   // the RX base
		op(kRegDioMapping1, kDio0RxDone);
		op(kRegIrqFlags, kIrqAll);
		op(kRegOpMode, kOpLoRa | (_rxContinuous ? kOpRxContinuous : kOpRxSingle));
		if (!_rxContinuous) {
			// A packet that begins inside the window is received whole, however
			// long after the window it ends: allow for the longest one.
			_deadlineMs = (uint32_t)(((uint64_t)lora::symbolUs(_cfg.sf, _cfg.bw) * _rxSymbols) / 1000u) +
			              lora::timeOnAirUs(_cfg, kMaxPayload) / 1000u + kRxMarginMs;
		}
		DBG_EVENT("rfm95", "rx", (int32_t)(_cfg.freqHz / 1000u), _cfg.sf, (int32_t)_cfg.bw, _rxSymbols, _cfg.invertIq ? 1 : 0);
		_req = Req::None;
		_irqPending = false;
		runOps(rfm95_rx_wait, nowMs);
		break;
	case Req::Standby:
		_rxContinuous = false;
		op(kRegOpMode, kOpLoRa | kOpStandby);
		op(kRegIrqFlags, kIrqAll);
		DBG_EVENT("rfm95", "standby");
		_req = Req::None;
		runOps(rfm95_idle, nowMs);
		break;
	case Req::Sleep:
		op(kRegOpMode, kOpLoRa | kOpSleep);
		DBG_EVENT("rfm95", "sleep");
		_req = Req::None;
		runOps(rfm95_idle, nowMs);
		break;
	case Req::None:
		break;
	}
	_startMs = nowMs;
	_lastPollMs = nowMs;
}

// While transmitting or receiving: true when it is time to read the IRQ
// flags (an interrupt came, or a poll is due). Fails a TX or RX single
// that has gone on far too long.
template <typename TTransport>
bool rfm95<TTransport>::waitFlags(uint32_t nowMs, uint32_t deadlineMs) {
	if (deadlineMs && nowMs - _startMs > deadlineMs) {
		DBG_FAULT("rfm95", this->_state == rfm95_tx_wait ? "tx-stuck" : "rx-stuck", (int32_t)(nowMs - _startMs));
		this->fail(nowMs);
		return false;
	}
	if (_irqPending || nowMs - _lastPollMs >= _param.pollMs) {
		_lastPollMs = nowMs;
		return true;
	}
	const uint32_t left = _param.pollMs - (nowMs - _lastPollMs);
	this->sleep(left);
	return false;
}

// ---- the state machine ----

template <typename TTransport>
void rfm95<TTransport>::main(uint32_t nowMs) {
	using namespace sx1276;
	switch (this->_state) {
	case rfm95_init:
		_rxContinuous = false;
		_req = Req::None;
		this->enter(rfm95_read_version, nowMs);
		break;

	case rfm95_read_version:
		this->issued(this->readRegs(kRegVersion, _regs, 1), rfm95_wait_version, nowMs);
		break;
	case rfm95_wait_version:
		if (this->landed(nowMs)) {
			if (_regs[0] != kVersion) {
				DBG_FAULT("rfm95", "version", _regs[0]);
				this->fail(nowMs);
				break;
			}
			// LoRa mode can only be chosen in sleep.
			opsClear();
			op(kRegOpMode, kOpSleep);
			op(kRegOpMode, kOpLoRa | kOpSleep);
			op(kRegOpMode, kOpLoRa | kOpStandby);
			op3(kRegFifoTxBaseAddr, 2, 0x00, 0x00, 0);   // TX and RX base both 0: one packet at a time
			op(kRegLna, kLnaMaxGainBoost);
			op(kRegModemConfig3, kAgcAuto);
			op(kRegSyncWord, _param.syncWord);
			op(kRegMaxPayloadLength, 0xFF);
			op(kRegIrqFlagsMask, 0x00);
			op(kRegIrqFlags, kIrqAll);
			runOps(rfm95_verify_mode, nowMs);
		}
		break;

	case rfm95_write_ops: {
		const Op& o = _ops[_opPos];
		this->issued(this->writeRegs(o.reg, o.ptr ? o.ptr : o.v, o.len), rfm95_wait_write_ops, nowMs);
		break;
	}
	case rfm95_wait_write_ops:
		if (this->landed(nowMs)) {
			if (++_opPos < _opCount) this->enter(rfm95_write_ops, nowMs);
			else {
				this->enter(_afterOps, nowMs);
				_startMs = nowMs;      // the TX/RX runs from here
				_lastPollMs = nowMs;
			}
		}
		break;

	case rfm95_verify_mode:
		this->issued(this->readRegs(kRegOpMode, _regs, 1), rfm95_wait_verify_mode, nowMs);
		break;
	case rfm95_wait_verify_mode:
		if (this->landed(nowMs)) {
			if (_regs[0] != (kOpLoRa | kOpStandby)) {
				DBG_FAULT("rfm95", "mode", _regs[0]);
				this->fail(nowMs);
			} else {
				this->enter(rfm95_verify_sync, nowMs);
			}
		}
		break;
	case rfm95_verify_sync:
		this->issued(this->readRegs(kRegSyncWord, _regs, 1), rfm95_wait_verify_sync, nowMs);
		break;
	case rfm95_wait_verify_sync:
		if (this->landed(nowMs)) {
			if (_regs[0] != _param.syncWord) {
				DBG_FAULT("rfm95", "sync", _regs[0]);
				this->fail(nowMs);
			} else {
				DBG_EVENT("rfm95", "ready");
				this->enter(rfm95_idle, nowMs);
			}
		}
		break;

	case rfm95_idle:
		if (_req != Req::None) startRequest(nowMs);
		else this->sleep(kIdleSleepMs);
		break;

	case rfm95_tx_wait:
	case rfm95_rx_wait:
		if (this->_state == rfm95_rx_wait && _req != Req::None) {   // a new request ends RX continuous
			startRequest(nowMs);
			break;
		}
		if (waitFlags(nowMs, _deadlineMs)) {
			_afterOps = this->_state;   // where read_status came from
			this->enter(rfm95_read_status, nowMs);
		}
		break;

	case rfm95_read_status:
		this->issued(this->readRegs(kRegFifoRxCurrentAddr, _regs, 4), rfm95_wait_status, nowMs);
		break;
	case rfm95_wait_status:
		if (this->landed(nowMs)) {
			const uint8_t flags = _regs[2];
			const bool fromIrq = _irqPending;
			const uint32_t when = fromIrq ? _irqTicks : nowTicks(nowMs);
			_irqPending = false;
			if (_afterOps == rfm95_tx_wait) {
				if (flags & kIrqTxDone) {
					++_st.txDone;
					push(rfm95_ev_tx_done, when, 0, 0, 0);
					DBG_EVENT("rfm95", "tx-done", (int32_t)(nowMs - _startMs));
					opsClear();
					op(kRegIrqFlags, kIrqAll);   // the chip is back in standby by itself
					runOps(rfm95_idle, nowMs);
				} else {
					this->enter(rfm95_tx_wait, nowMs);
				}
				break;
			}
			// Receiving.
			if (flags & kIrqRxTimeout) {
				++_st.rxTimeouts;
				push(rfm95_ev_rx_timeout, when, 0, 0, 0);
				DBG_EVENT("rfm95", "rx-timeout");
				opsClear();
				op(kRegIrqFlags, kIrqAll);   // RX single is back in standby by itself
				runOps(rfm95_idle, nowMs);
			} else if (flags & kIrqRxDone) {
				if (flags & kIrqCrcError) {
					++_st.crcErrors;
					push(rfm95_ev_crc_error, when, 0, 0, 0);
					DBG_FAULT("rfm95", "crc");
					opsClear();
					op(kRegIrqFlags, kIrqAll);
					runOps(_rxContinuous ? rfm95_rx_wait : rfm95_idle, nowMs);
				} else {
					_rxTicks = when;
					_rxAddr = _regs[0];
					_rxLen = _regs[3];
					_rxPos = 0;
					this->enter(rfm95_read_signal, nowMs);
				}
			} else {
				this->enter(rfm95_rx_wait, nowMs);
			}
		}
		break;

	case rfm95_read_signal:
		this->issued(this->readRegs(kRegPktSnrValue, _regs, 2), rfm95_wait_signal, nowMs);
		break;
	case rfm95_wait_signal:
		if (this->landed(nowMs)) {
			_rxSnrRaw = _regs[0];
			_rxRssiRaw = _regs[1];
			if (_rxLen == 0) {   // an empty packet: nothing to read
				this->enter(rfm95_read_fifo, nowMs);
				break;
			}
			// Point the FIFO at the packet; read_fifo follows.
			opsClear();
			op(kRegFifoAddrPtr, _rxAddr);
			runOps(rfm95_read_fifo, nowMs);
		}
		break;

	case rfm95_read_fifo:
		if (_rxPos >= _rxLen) {   // all read: deliver, clear, and carry on
			const int8_t snr = lora::packetSnrDb(_rxSnrRaw);
			const int16_t rssi = lora::packetRssiDbm(_rxRssiRaw, _rxSnrRaw, _cfg.freqHz);
			++_st.rxDone;
			push(rfm95_ev_rx_done, _rxTicks, rssi, snr, _rxLen);
			DBG_EVENT("rfm95", "rx-done", _rxLen, rssi, snr);
			opsClear();
			op(kRegIrqFlags, kIrqAll);
			runOps(_rxContinuous ? rfm95_rx_wait : rfm95_idle, nowMs);
			break;
		}
		{
			const uint8_t left = (uint8_t)(_rxLen - _rxPos);
			const uint8_t n = left < ISensorTransport::kMaxWriteLen ? left : ISensorTransport::kMaxWriteLen;
			this->issued(this->readRegs(kRegFifo, _rxBuf + _rxPos, n), rfm95_wait_fifo, nowMs);
		}
		break;
	case rfm95_wait_fifo:
		if (this->landed(nowMs)) {
			const uint8_t left = (uint8_t)(_rxLen - _rxPos);
			_rxPos = (uint8_t)(_rxPos + (left < ISensorTransport::kMaxWriteLen ? left : ISensorTransport::kMaxWriteLen));
			this->enter(rfm95_read_fifo, nowMs);
		}
		break;

	case rfm95_error:
		if (this->errorCleared(nowMs, kErrorBackoffMs)) this->enter(rfm95_init, nowMs);
		break;

	default:
		this->enter(rfm95_init, nowMs);
		break;
	}
}

template <typename TTransport>
void rfm95<TTransport>::onFail() {
	++_st.faults;
	_req = Req::None;
	_rxContinuous = false;
	_irqPending = false;
	push(rfm95_ev_fault, 0, 0, 0, 0);
}
