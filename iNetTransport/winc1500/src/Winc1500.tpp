/*
 * Winc1500.tpp
 *
 *  winc1500<TTransport>'s member definitions, included at the end of
 *  Winc1500.h. See there, and WincProtocol.h for where the numbers come
 *  from.
 */

#include "../inc/Winc1500.h"
#include <string.h>

namespace WINC1500 {

using namespace WINC;

template <typename TTransport>
template <typename... TArgs>
winc1500<TTransport>::winc1500(const winc_param_t &param, TArgs&&... transportArgs)
	: TTransport(std::forward<TArgs>(transportArgs)...), _param(param) {
	_nSocks = _param.sockets < 1 ? 1 : (_param.sockets > tcp_sockets ? tcp_sockets : _param.sockets);
	if (_param.pollMs == 0) _param.pollMs = 1;
	memset(_ssid, 0, sizeof _ssid);
	memset(_pass, 0, sizeof _pass);
	memset(_dnsName, 0, sizeof _dnsName);
	memset(_tx, 0, sizeof _tx);
	for (uint8_t w = 0; w < tcp_sockets; ++w) { _wOwner[w] = kFreeW; _wSession[w] = 0; }
}

// ---------------------------------------------------------------------
// Requests from the interface
// ---------------------------------------------------------------------

template <typename TTransport>
void winc1500<TTransport>::configure(const NetConfig &cfg) {
	const bool wasRunning = _configured;
	_cfg = cfg;
	_configured = true;
	if (wasRunning) dropSockets(0);	// every open socket is lost
	restart(0);
	_error = winc_error_t::none;
}

template <typename TTransport>
bool winc1500<TTransport>::connect(uint8_t s, const IpAddress &ip, uint16_t port, uint16_t localPort) {
	(void)localPort;	// the module picks it
	if (!_configured || s >= _nSocks || port == 0) return false;
	Sock &k = _sock[s];
	if (k.w >= 0) queueClose(k.w);	// whatever it was doing: dropped, no event
	if (_rxOwner == static_cast<int8_t>(s)) { _rxLen = _rxPos = 0; _rxOwner = -1; }
	if (_heldSock == static_cast<int8_t>(s)) _heldSock = -1;
	if (_txPending && _txIdx == static_cast<int8_t>(s) && (_txFor == TxFor::connect || _txFor == TxFor::send || _txFor == TxFor::recv)) _txPending = false;
	k = Sock();
	k.req = Sock::ConnectReq;
	k.ip = ip;
	k.port = port;
	return true;
}

template <typename TTransport>
bool winc1500<TTransport>::listen(uint8_t s, uint16_t port) {
	if (!_configured || s >= _nSocks || port == 0) return false;
	Sock &k = _sock[s];
	if (k.w >= 0) queueClose(k.w);
	if (_rxOwner == static_cast<int8_t>(s)) { _rxLen = _rxPos = 0; _rxOwner = -1; }
	if (_heldSock == static_cast<int8_t>(s)) _heldSock = -1;
	if (_txPending && _txIdx == static_cast<int8_t>(s) && (_txFor == TxFor::connect || _txFor == TxFor::send || _txFor == TxFor::recv)) _txPending = false;
	k = Sock();
	k.req = Sock::ListenReq;
	k.port = port;
	return true;
}

template <typename TTransport>
void winc1500<TTransport>::close(uint8_t s) {
	if (s >= _nSocks) return;
	Sock &k = _sock[s];
	if (k.w >= 0) queueClose(k.w);	// CLOSE has no reply: the socket is free once it's sent
	if (_rxOwner == static_cast<int8_t>(s)) { _rxLen = _rxPos = 0; _rxOwner = -1; }
	if (_heldSock == static_cast<int8_t>(s)) _heldSock = -1;
	if (_txPending && _txIdx == static_cast<int8_t>(s) && (_txFor == TxFor::connect || _txFor == TxFor::send || _txFor == TxFor::recv)) _txPending = false;
	k = Sock();
	k.closedEvent = true;	// reported from poll()
}

template <typename TTransport>
bool winc1500<TTransport>::resolve(const char *name) {
	if (!_configured || name == nullptr) return false;
	const size_t n = strlen(name);
	if (n == 0 || n >= hostname_max) return false;
	memcpy(_dnsName, name, n + 1);
	_dnsReq = true;
	_dnsSent = false;
	return true;
}

template <typename TTransport>
bool winc1500<TTransport>::requestTime(const char *server) {
	// The firmware's SNTP client uses its own servers; the one named is
	// not used (19.5 has no request to set it).
	if (!_configured || server == nullptr) return false;
	_timeReq = true;
	_timeSent = false;
	_timeAt = 0;
	_timeNext = 0;
	return true;
}

template <typename TTransport>
void winc1500<TTransport>::join(const char *ssid, const char *passphrase) {
	const size_t ns = ssid ? strlen(ssid) : 0;
	const size_t np = passphrase ? strlen(passphrase) : 0;
	_joinErr = 0;
	if (ns == 0 || ns > max_ssid_len || (np != 0 && (np < 8 || np > max_psk_len))) {
		_ssid[0] = 0;	// reported as JoinFailed from poll()
		_joinReq = true;
		return;
	}
	memcpy(_ssid, ssid, ns + 1);
	memset(_pass, 0, sizeof _pass);
	if (np) memcpy(_pass, passphrase, np);
	_joinReq = true;
	_rejoin = false;
	if (_assoc || _joining) { _leaveReq = true; _leaveQuiet = true; }	// leave the network we're on first
}

template <typename TTransport>
void winc1500<TTransport>::leave() {
	_joinReq = false;
	_rejoin = false;
	_leaveReq = true;
	_leaveQuiet = false;
}

// ---------------------------------------------------------------------
// The SPI access engine
// ---------------------------------------------------------------------

template <typename TTransport>
void winc1500<TTransport>::startOp(OpKind k, uint32_t addr, uint32_t value, uint8_t *rbuf,
                                   const uint8_t *wbuf, uint16_t len) {
	_op = Op();
	_op.kind = k;
	_op.addr = addr;
	_op.value = value;
	_op.rbuf = rbuf;
	_op.wbuf = wbuf;
	_op.len = len;
	_opActive = true;
}

template <typename TTransport>
uint8_t winc1500<TTransport>::buildCommand() {
	const uint32_t a = _op.addr;
	// A one-byte block is sent as two: Microchip's workaround for the
	// chip's single-byte transfers.
	const uint32_t size = _op.len == 1 ? 2u : _op.len;
	uint8_t n = 0;
	switch (_op.kind) {
	case OpKind::reg_read:
		_cmd[0] = cmd_single_read;
		_cmd[1] = static_cast<uint8_t>(a >> 16); _cmd[2] = static_cast<uint8_t>(a >> 8); _cmd[3] = static_cast<uint8_t>(a);
		n = 4;
		break;
	case OpKind::reg_write:
		_cmd[0] = cmd_single_write;
		_cmd[1] = static_cast<uint8_t>(a >> 16); _cmd[2] = static_cast<uint8_t>(a >> 8); _cmd[3] = static_cast<uint8_t>(a);
		_cmd[4] = static_cast<uint8_t>(_op.value >> 24); _cmd[5] = static_cast<uint8_t>(_op.value >> 16);
		_cmd[6] = static_cast<uint8_t>(_op.value >> 8); _cmd[7] = static_cast<uint8_t>(_op.value);
		n = 8;
		break;
	case OpKind::block_read:
	case OpKind::block_write:
		_cmd[0] = _op.kind == OpKind::block_read ? cmd_dma_ext_read : cmd_dma_ext_write;
		_cmd[1] = static_cast<uint8_t>(a >> 16); _cmd[2] = static_cast<uint8_t>(a >> 8); _cmd[3] = static_cast<uint8_t>(a);
		_cmd[4] = static_cast<uint8_t>(size >> 16); _cmd[5] = static_cast<uint8_t>(size >> 8); _cmd[6] = static_cast<uint8_t>(size);
		n = 7;
		break;
	default:
		break;
	}
	if (!_crcOff) {
		_cmd[n] = static_cast<uint8_t>(crc7(0x7F, _cmd, n) << 1);
		++n;
	}
	return n;
}

template <typename TTransport>
bool winc1500<TTransport>::issueWrite(const uint8_t *hdr, uint8_t hl, const uint8_t *data, size_t n, uint32_t now) {
	if (!TTransport::beginWrite(hdr, hl, data, n)) {
		if (!_op.xfer && _op.xferAt == 0) _op.xferAt = now ? now : 1;	// refused: retried until the bus timeout
		return false;
	}
	_op.xfer = true;
	_op.xferAt = now;
	_stats.transfers++;
	return true;
}

template <typename TTransport>
bool winc1500<TTransport>::issueRead(uint8_t *data, size_t n, uint32_t now) {
	if (!TTransport::beginRead(nullptr, 0, data, n)) {
		if (!_op.xfer && _op.xferAt == 0) _op.xferAt = now ? now : 1;
		return false;
	}
	_op.xfer = true;
	_op.xferAt = now;
	_stats.transfers++;
	return true;
}

// One step of a register or block access. Returns false on a protocol
// error; sets again when the next step can run at once.
template <typename TTransport>
bool winc1500<TTransport>::opStep(uint32_t now, bool &again) {
	static const uint8_t token = data_token_single;
	static const uint8_t zero2[2] = {0, 0};
	again = false;
	switch (_op.step) {
	case 0: {	// the command
		const uint8_t n = buildCommand();
		if (issueWrite(nullptr, 0, _cmd, n, now)) { _op.step = 1; _op.polls = 0; }
		return true;
	}
	case 1:		// its echo, a byte at a time
	case 3:		// then the state byte
	case 5:		// then (reads) the data's start
		if (issueRead(_b, 1, now)) _op.step++;
		return true;
	case 2:
		if (_b[0] == _cmd[0]) { _op.step = 3; _op.polls = 0; again = true; return true; }
		if (++_op.polls >= resp_polls) return false;
		_op.step = 1; again = true;
		return true;
	case 4:
		if (_b[0] == 0) {
			_op.polls = 0;
			if (_op.kind == OpKind::reg_write) { _op.step = 20; again = true; return true; }
			_op.step = _op.kind == OpKind::block_write ? 7 : 5;
			again = true;
			return true;
		}
		if (++_op.polls >= resp_polls) return false;
		_op.step = 3; again = true;
		return true;
	case 6:
		if ((_b[0] >> 4) == 0x0F) { _op.step = 8; again = true; return true; }
		if (++_op.polls >= resp_polls) return false;
		_op.step = 5; again = true;
		return true;
	case 7: {	// a block write's data packet
		const uint8_t *d = _op.wbuf;
		size_t n = _op.len;
		if (n == 1) { _tmp2[0] = _op.wbuf[0]; _tmp2[1] = 0; d = _tmp2; n = 2; }
		if (issueWrite(&token, 1, d, n, now)) _op.step = 11;
		return true;
	}
	case 8:		// the data
		if (_op.kind == OpKind::reg_read) {
			if (issueRead(_b, 4, now)) _op.step = 9;
		} else if (_op.len == 1) {
			if (issueRead(_tmp2, 2, now)) _op.step = 9;
		} else {
			if (issueRead(_op.rbuf, _op.len, now)) _op.step = 9;
		}
		return true;
	case 9:		// and its CRC, while the CRC is on (not checked: Microchip's driver doesn't)
		if (_crcOff) { _op.step = 10; again = true; return true; }
		if (issueRead(_crcBuf, 2, now)) _op.step = 10;
		return true;
	case 10:
		if (_op.kind == OpKind::reg_read) _opValue = le32(_b);
		else if (_op.len == 1) _op.rbuf[0] = _tmp2[0];
		_op.step = 20; again = true;
		return true;
	case 11:	// the data's CRC (zeros: the module isn't checking it)
		if (_crcOff) { _op.step = 12; again = true; return true; }
		if (issueWrite(nullptr, 0, zero2, 2, now)) _op.step = 12;
		return true;
	case 12:	// the module's answer to the data: C3 00, after a byte with the CRC off
		if (issueRead(_b, _crcOff ? 3 : 2, now)) _op.step = 13;
		return true;
	case 13: {
		const uint8_t n = _crcOff ? 3 : 2;
		if (_b[n - 2] != data_write_ack || _b[n - 1] != 0) return false;
		_op.step = 20; again = true;
		return true;
	}
	default:
		return true;
	}
}

// The reset command between tries: CF FF FF FF, a byte skipped, the echo
// and the state polled (and not insisted on), a millisecond's pause.
template <typename TTransport>
bool winc1500<TTransport>::resetStep(uint32_t now, bool &again) {
	again = false;
	switch (_op.step) {
	case 0: {
		_cmd[0] = cmd_reset; _cmd[1] = 0xFF; _cmd[2] = 0xFF; _cmd[3] = 0xFF;
		uint8_t n = 4;
		if (!_crcOff) { _cmd[4] = static_cast<uint8_t>(crc7(0x7F, _cmd, 4) << 1); n = 5; }
		if (issueWrite(nullptr, 0, _cmd, n, now)) { _op.step = 1; _op.polls = 0; }
		return true;
	}
	case 1:
	case 2:
	case 4:
		if (issueRead(_b, 1, now)) _op.step = _op.step == 1 ? 2 : (_op.step == 2 ? 3 : 5);
		return true;
	case 3:
		if (_b[0] == cmd_reset || ++_op.polls >= resp_polls) { _op.step = 4; _op.polls = 0; }
		else _op.step = 2;
		again = true;
		return true;
	case 5:
		if (_b[0] == 0 || ++_op.polls >= resp_polls) {
			_op.step = 0;
			_op.polls = 0;
			_op.resetting = false;
			_op.delaying = true;
			_op.delayUntil = now + 1;
		} else {
			_op.step = 4;
		}
		again = true;
		return true;
	default:
		return true;
	}
}

// A protocol or bus error: reset the SPI and start the access again, up
// to spi_tries times. false: give up.
template <typename TTransport>
bool winc1500<TTransport>::opError(uint32_t now) {
	_op.xfer = false;
	if (++_op.tries >= spi_tries) return false;
	_stats.spiRetries++;
	_op.resetting = true;
	_op.step = 0;
	_op.polls = 0;
	_op.xferAt = 0;
	_op.delaying = true;	// Microchip's driver pauses a millisecond first
	_op.delayUntil = now + 1;
	return true;
}

template <typename TTransport>
typename winc1500<TTransport>::OpResult winc1500<TTransport>::runOp(uint32_t now) {
	for (int guard = 0; guard < 64; ++guard) {
		if (_op.xfer) {
			if (TTransport::isBusy()) {
				if (now - _op.xferAt > winc_bus_timeout_ms) {
					if (!opError(now)) return OpResult::failed;
					continue;
				}
				return OpResult::busy;
			}
			_op.xfer = false;
			_op.xferAt = 0;
			if (TTransport::lastOpFailed()) {
				if (!opError(now)) return OpResult::failed;
				continue;
			}
		}
		if (_op.delaying) {
			if (static_cast<int32_t>(now - _op.delayUntil) < 0) return OpResult::busy;
			_op.delaying = false;
		}
		if (!_op.resetting && _op.step == 20) {
			_opActive = false;
			return OpResult::done;
		}
		bool again = false;
		const bool ok = _op.resetting ? resetStep(now, again) : opStep(now, again);
		if (!ok) {
			if (!opError(now)) return OpResult::failed;
			continue;
		}
		if (!_op.xfer && !again && !_op.delaying && !(!_op.resetting && _op.step == 20)) {
			// The bus refused the transfer: try again until it times out.
			if (_op.xferAt != 0 && now - _op.xferAt > winc_bus_timeout_ms) {
				_op.xferAt = 0;
				if (!opError(now)) return OpResult::failed;
				continue;
			}
			return OpResult::busy;
		}
	}
	return OpResult::busy;
}

// ---------------------------------------------------------------------
// Start-up
// ---------------------------------------------------------------------

template <typename TTransport>
void winc1500<TTransport>::resetRuntime() {
	_proc = Proc::none;
	_pstep = 0;
	_opActive = false;
	_irq = false;
	_msgHeld = false;
	_heldSock = -1;
	_heldLeft = 0;
	_heldCounted = false;
	_txPending = false;
	_rxLen = _rxPos = 0;
	_rxOwner = -1;
	_ready = false;
	_setupStep = 0;
	for (uint8_t i = 0; i < winc_listeners; ++i) _lst[i] = Listener();
	for (uint8_t w = 0; w < tcp_sockets; ++w) { _wOwner[w] = kFreeW; _wSession[w] = 0; }
	_closeMask = 0;
	const bool hadJoin = _joining || _assoc || _joinReq;
	_assoc = _up = _joining = _leaving = false;
	_leaveReq = false;
	_joinReq = hadJoin && _ssid[0] != 0;	// join again once the module is back
	_rejoin = _rejoin || _joinReq;
	_rejoinAt = 0;
	_allocFails = 0;
	_dnsSent = false;
	_timeSent = false;
}

template <typename TTransport>
void winc1500<TTransport>::restart(uint32_t now) {
	resetRuntime();
	_crcOff = false;
	_crcProbeOff = false;
	_softResetDone = false;
	_confTries = 0;
	_delaying = false;
	enter(winc_state_t::reset_hold, now);
}

template <typename TTransport>
void winc1500<TTransport>::fail(winc_error_t e, uint32_t now) {
	_error = e;
	_stats.failures++;
	if (_state == winc_state_t::run) {
		if (_up) linkLost(now);
		dropSockets(now);
		devEvent(DeviceEvent::Failed);
	} else {
		devEvent(DeviceEvent::Failed);
	}
	resetRuntime();
	_state = winc_state_t::error;
	waitThen(now, winc_error_backoff_ms);
}

template <typename TTransport>
uint32_t winc1500<TTransport>::poll(uint32_t now) {
	if (!_configured) return 1000;
	if (_delaying) {
		if (static_cast<int32_t>(now - _delayUntil) < 0) return _delayUntil - now;
		_delaying = false;
	}
	if (_state == winc_state_t::error) {
		restart(now);
		return 0;
	}
	if (_state == winc_state_t::run) return runMain(now);
	return runStartup(now);
}

template <typename TTransport>
uint32_t winc1500<TTransport>::runStartup(uint32_t now) {
	typedef winc_state_t S;
	// Most states: start an access, and when it's done act on it.
	OpResult r = OpResult::done;
	auto access = [&]() -> bool {	// true once the access has finished well
		r = runOp(now);
		if (r == OpResult::failed) {
			fail(_state == S::spi_probe || _state == S::chip_id ? winc_error_t::no_chip : winc_error_t::bus, now);
			return false;
		}
		return r == OpResult::done;
	};
	switch (_state) {
	case S::reset_hold:
		if (_param.hardReset) {
			_param.hardReset(true);
			_state = S::reset_wait;
			waitThen(now, winc_reset_hold_ms);
		} else {
			enter(S::spi_probe, now);
		}
		return 0;
	case S::reset_wait:
		_param.hardReset(false);
		enter(S::spi_probe, now);
		waitThen(now, winc_reset_wait_ms);
		return 0;

	case S::spi_probe:
		// The protocol register with the CRC on, as after a reset; if
		// that fails, the module may still have it off from before.
		if (!_opActive) { readReg(reg_spi_protocol_config); return 0; }
		r = runOp(now);
		if (r == OpResult::busy) return 0;
		if (r == OpResult::failed) {
			if (!_crcProbeOff) { _crcProbeOff = true; _crcOff = true; _opActive = false; return 0; }
			fail(winc_error_t::no_chip, now);
			return 0;
		}
		if (!_crcProbeOff) {
			_cfgVal = (_opValue & ~(spi_cfg_crc_bits | spi_cfg_pkt_mask)) | spi_cfg_pkt_8k;
			enter(S::spi_crc_off, now);
		} else {
			enter(!_param.hardReset && !_softResetDone ? S::soft_reset : S::chip_id, now);
		}
		return 0;
	case S::spi_crc_off:
		if (!_opActive) { writeReg(reg_spi_protocol_config, _cfgVal); return 0; }	// sent with the CRC, the last time
		if (!access()) return 0;
		_crcOff = true;
		enter(!_param.hardReset && !_softResetDone ? S::soft_reset : S::chip_id, now);
		return 0;
	case S::soft_reset:
		// No reset pin: a global reset through the register, then the
		// SPI from the start (it comes back with the CRC on).
		if (!_opActive) { writeReg(reg_glb_reset, 0); return 0; }
		if (!access()) return 0;
		_softResetDone = true;
		_crcOff = false;
		_crcProbeOff = false;
		enter(S::spi_probe, now);
		waitThen(now, winc_soft_reset_wait_ms);
		return 0;
	case S::chip_id:
		if (!_opActive) { readReg(reg_chip_id); return 0; }
		if (!access()) return 0;
		_chipId = _opValue;
		{
			const uint16_t rev = static_cast<uint16_t>(_chipId & 0xFF0);
			if ((_chipId & 0xFFF00000u) != 0x00100000u || (rev != rev_3a0 && rev != rev_b0)) {
				fail(winc_error_t::wrong_chip, now);
				return 0;
			}
		}
		enter(S::pkt_size_read, now);
		return 0;
	case S::pkt_size_read:
		if (!_opActive) { readReg(reg_spi_protocol_config); return 0; }
		if (!access()) return 0;
		_cfgVal = (_opValue & ~spi_cfg_pkt_mask) | spi_cfg_pkt_8k;
		enter(S::pkt_size_write, now);
		return 0;
	case S::pkt_size_write:
		if (!_opActive) { writeReg(reg_spi_protocol_config, _cfgVal); return 0; }
		if (!access()) return 0;
		enter(S::efuse_wait, now);
		return 0;

	case S::efuse_wait:
		if (!_opActive) { readReg(reg_efuse_done); return 0; }
		if (!access()) return 0;
		if (_opValue & 0x80000000u) { enter(S::wait_host, now); return 0; }
		if (now - _phaseStart > winc_boot_timeout_ms) { fail(winc_error_t::boot_timeout, now); return 0; }
		waitThen(now, 1);
		return 1;
	case S::wait_host:
		if (!_opActive) { readReg(reg_wait_for_host); return 0; }
		if (!access()) return 0;
		enter((_opValue & 1u) ? S::host_version : S::bootrom_wait, now);
		return 0;
	case S::bootrom_wait:
		if (!_opActive) { readReg(reg_bootrom); return 0; }
		if (!access()) return 0;
		if (_opValue == finish_boot_rom) { enter(S::host_version, now); return 0; }
		if (now - _phaseStart > winc_boot_timeout_ms) { fail(winc_error_t::boot_timeout, now); return 0; }
		waitThen(now, 1);
		return 1;
	case S::host_version:
		if (!_opActive) { writeReg(reg_nmi_state, host_version_info); return 0; }
		if (!access()) return 0;
		_confTries = 0;
		enter(S::conf_write, now);
		return 0;
	case S::conf_write:
		_cfgVal = gp1_reserved1 | ((_chipId & 0xFFF) >= rev_3a0 ? gp1_use_pmu : 0u);
		if (!_opActive) { writeReg(reg_gp_1, _cfgVal); return 0; }
		if (!access()) return 0;
		enter(S::conf_check, now);
		return 0;
	case S::conf_check:
		if (!_opActive) { readReg(reg_gp_1); return 0; }
		if (!access()) return 0;
		if (_opValue == _cfgVal) { enter(S::start_firmware, now); return 0; }
		if (++_confTries >= spi_tries) { fail(winc_error_t::bus, now); return 0; }
		enter(S::conf_write, now);
		return 0;
	case S::start_firmware:
		if (!_opActive) { writeReg(reg_bootrom, start_firmware); return 0; }
		if (!access()) return 0;
		enter(S::firmware_wait, now);
		waitThen(now, 2);
		return 2;
	case S::firmware_wait:
		if (!_opActive) { readReg(reg_nmi_state); return 0; }
		if (!access()) return 0;
		if (_opValue == finish_init_state) { enter(S::firmware_ack, now); return 0; }
		if (now - _phaseStart > winc_firmware_timeout_ms) { fail(winc_error_t::firmware_timeout, now); return 0; }
		waitThen(now, 2);
		return 2;
	case S::firmware_ack:
		if (!_opActive) { writeReg(reg_nmi_state, 0); return 0; }
		if (!access()) return 0;
		enter(S::irq_mux_read, now);
		return 0;

	case S::irq_mux_read:
	case S::irq_en_read: {
		const bool mux = _state == S::irq_mux_read;
		if (!_opActive) { readReg(mux ? reg_pin_mux_0 : reg_intr_enable); return 0; }
		if (!access()) return 0;
		_cfgVal = _opValue | (mux ? (1u << 8) : (1u << 16));
		enter(mux ? S::irq_mux_write : S::irq_en_write, now);
		return 0;
	}
	case S::irq_mux_write:
	case S::irq_en_write: {
		const bool mux = _state == S::irq_mux_write;
		if (!_opActive) { writeReg(mux ? reg_pin_mux_0 : reg_intr_enable, _cfgVal); return 0; }
		if (!access()) return 0;
		enter(mux ? S::irq_en_read : S::gp2_read, now);
		return 0;
	}

	case S::gp2_read:
		if (!_opActive) { readReg(reg_gp_2); return 0; }
		if (!access()) return 0;
		if (_opValue == 0) { fail(winc_error_t::firmware_too_old, now); return 0; }	// 19.3 and before
		_cfgVal = _opValue;
		enter(S::gpregs_read, now);
		return 0;
	case S::gpregs_read:
		if (!_opActive) { readBlock(_cfgVal | data_mem_base, _gp, sizeof _gp); return 0; }
		if (!access()) return 0;
		if ((le32(_gp + 4) & 0xFFFF) == 0) { fail(winc_error_t::firmware_too_old, now); return 0; }
		enter(S::rev_read, now);
		return 0;
	case S::rev_read:
		if (!_opActive) { readBlock((le32(_gp + 4) & 0xFFFF) | data_mem_base, _rev, rev_bytes); return 0; }
		if (!access()) return 0;
		_fw.major = _rev[4];
		_fw.minor = _rev[5];
		_fw.patch = _rev[6];
		if (makeVersion(_rev[4], _rev[5], _rev[6]) < min_firmware_version) {
			fail(winc_error_t::firmware_too_old, now);
			return 0;
		}
		if (makeVersion(_rev[7], _rev[8], _rev[9]) > host_driver_version) {
			fail(winc_error_t::firmware_too_new, now);
			return 0;
		}
		enter(S::mac_read, now);
		return 0;
	case S::mac_read:
		if ((le32(_gp) & 0xFFFF) == 0) {
			_mac = MacAddress();
		} else {
			if (!_opActive) { readBlock((le32(_gp) & 0xFFFF) | data_mem_base, _macBuf, 6); return 0; }
			if (!access()) return 0;
			memcpy(_mac.b, _macBuf, 6);
		}
		_stats.inits++;
		_error = winc_error_t::none;
		enter(S::run, now);
		_nextCheck = now;
		_setupStep = 0;
		_ready = false;
		return 0;
	default:
		return 1000;
	}
}

// ---------------------------------------------------------------------
// The running module
// ---------------------------------------------------------------------

template <typename TTransport>
int8_t winc1500<TTransport>::allocW(uint8_t owner) {
	for (uint8_t w = 0; w < tcp_sockets; ++w) {
		if (_wOwner[w] == kFreeW && !(_closeMask & (1u << w))) {
			_wOwner[w] = owner;
			_wSession[w] = nextSession();
			return static_cast<int8_t>(w);
		}
	}
	return -1;
}

template <typename TTransport>
void winc1500<TTransport>::queueClose(int8_t w) {
	if (w < 0 || w >= static_cast<int8_t>(tcp_sockets)) return;
	_wOwner[w] = kStrayW;
	_closeMask = static_cast<uint8_t>(_closeMask | (1u << w));
}

template <typename TTransport>
void winc1500<TTransport>::socketFailed(uint8_t s, SocketEvent ev) {
	Sock &k = _sock[s];
	if (k.w >= 0) queueClose(k.w);
	if (_rxOwner == static_cast<int8_t>(s)) { _rxLen = _rxPos = 0; _rxOwner = -1; }
	if (_heldSock == static_cast<int8_t>(s)) _heldSock = -1;
	if (_txPending && _txIdx == static_cast<int8_t>(s) && (_txFor == TxFor::connect || _txFor == TxFor::send || _txFor == TxFor::recv)) _txPending = false;
	k = Sock();
	emit(s, ev);
}

template <typename TTransport>
void winc1500<TTransport>::listenerFailed(uint8_t l) {
	Listener &L = _lst[l];
	if (L.w >= 0) queueClose(L.w);
	const uint16_t port = L.port;
	L = Listener();
	for (uint8_t s = 0; s < _nSocks; ++s) {
		if (_sock[s].mode == Sock::Listening && _sock[s].port == port) socketFailed(s, SocketEvent::Failed);
	}
}

// Every connection and listening socket is lost (link down, a new
// address, the module restarting). Connects and listens not yet
// started stay: they wait for the next address.
template <typename TTransport>
void winc1500<TTransport>::dropSockets(uint32_t now) {
	(void)now;
	for (uint8_t l = 0; l < winc_listeners; ++l) {
		if (_lst[l].w >= 0) queueClose(_lst[l].w);
		_lst[l] = Listener();
	}
	for (uint8_t s = 0; s < _nSocks; ++s) {
		if (_sock[s].mode != Sock::Idle) socketFailed(s, SocketEvent::Failed);
	}
	_rxLen = _rxPos = 0;
	_rxOwner = -1;
	_heldSock = -1;
}

template <typename TTransport>
void winc1500<TTransport>::linkLost(uint32_t now) {
	_up = false;
	_net = NetConfig();
	devEvent(DeviceEvent::LinkDown);
	if (_host) _host->addressChanged(_net);
	dropSockets(now);
}

template <typename TTransport>
void winc1500<TTransport>::deliverRx() {
	if (_rxLen == 0 || _host == nullptr) return;
	if (_rxOwner < 0 || _sock[_rxOwner].mode != Sock::Established) {	// nobody to give it to
		_rxLen = _rxPos = 0;
		_rxOwner = -1;
		return;
	}
	const size_t room = _host->rxSpace(static_cast<uint8_t>(_rxOwner));
	size_t n = static_cast<size_t>(_rxLen - _rxPos);
	if (n > room) n = room;
	if (n == 0) return;
	_host->rxDeliver(static_cast<uint8_t>(_rxOwner), _rx + _rxPos, n);
	_stats.rxBytes += static_cast<uint32_t>(n);
	_rxPos = static_cast<uint16_t>(_rxPos + n);
	if (_rxPos == _rxLen) { _rxLen = _rxPos = 0; _rxOwner = -1; }
}

template <typename TTransport>
void winc1500<TTransport>::checkTimers(uint32_t now) {
	// Wi-Fi.
	if (_joining && !_up && now - _joinAt > (_assoc ? winc_dhcp_timeout_ms : winc_join_timeout_ms)) {
		_joining = false;
		_joinErr = _assoc ? 0 : err_join_fail;
		if (_rejoin) { _joinReq = true; _rejoinAt = now + winc_rejoin_ms; }
		else devEvent(DeviceEvent::JoinFailed);
		_leaveReq = true;	// stop the module trying
		_leaveQuiet = true;
	}
	if (_leaving && now - _leaveAt > winc_listen_timeout_ms) {	// the module never said it had left
		_leaving = false;
		if (!_leaveQuiet) devEvent(DeviceEvent::LinkDown);
	}
	// Sockets.
	for (uint8_t s = 0; s < _nSocks; ++s) {
		Sock &k = _sock[s];
		if (k.mode == Sock::Connecting && k.sent && now - k.since > winc_connect_timeout_ms) socketFailed(s, SocketEvent::Failed);
		else if (k.mode == Sock::Established && k.sendInFlight && now - k.since > winc_send_timeout_ms) socketFailed(s, SocketEvent::Failed);
	}
	for (uint8_t l = 0; l < winc_listeners; ++l) {
		Listener &L = _lst[l];
		if ((L.st == Listener::BindSent || L.st == Listener::ListenSent) && now - L.since > winc_listen_timeout_ms) listenerFailed(l);
	}
	// DNS and time.
	if (_dnsReq && _dnsSent && now - _dnsAt > winc_dns_timeout_ms) {
		_dnsReq = _dnsSent = false;
		if (_host) _host->resolved(false, IpAddress());
	}
	if (_timeReq && _timeAt != 0 && now - _timeAt > winc_sntp_wait_ms) {
		_timeReq = _timeSent = false;
		if (_host) _host->timeReceived(false, 0, now);
	}
}

// Bookkeeping that needs no bus: events to report, listens to attach to
// a listening socket, sockets the peer closed whose data has all gone.
template <typename TTransport>
void winc1500<TTransport>::progress(uint32_t now) {
	for (uint8_t s = 0; s < _nSocks; ++s) {
		Sock &k = _sock[s];
		if (k.closedEvent) { k.closedEvent = false; emit(s, SocketEvent::Closed); }
		if (k.ending && !(_rxOwner == static_cast<int8_t>(s) && _rxLen != 0) && _heldSock != static_cast<int8_t>(s)) {
			const SocketEvent ev = k.endEv;
			socketFailed(s, ev);	// the reader has every byte: now the close
		}
	}
	if (_joinReq && _ssid[0] == 0) {	// join() with a bad SSID or passphrase
		_joinReq = false;
		devEvent(DeviceEvent::JoinFailed);
	}
	if (_leaveReq && !_assoc && !_joining && !_leaving) {	// nothing to leave
		_leaveReq = false;
		if (!_leaveQuiet) devEvent(DeviceEvent::LinkDown);
	}
	if (_dnsReq && !_dnsSent) {
		IpAddress ip;
		if (IpAddress::parse(_dnsName, ip)) {
			_dnsReq = false;
			if (_host) _host->resolved(true, ip);
		}
	}
	if (!_up) return;
	// Listens: each port has one of the module's sockets listening.
	for (uint8_t s = 0; s < _nSocks; ++s) {
		Sock &k = _sock[s];
		if (k.req != Sock::ListenReq) continue;
		int l = -1;
		for (uint8_t i = 0; i < winc_listeners; ++i) if (_lst[i].st != Listener::Free && _lst[i].port == k.port) l = i;
		if (l < 0) {
			for (uint8_t i = 0; i < winc_listeners && l < 0; ++i) {
				if (_lst[i].st == Listener::Free) {
					const int8_t w = allocW(static_cast<uint8_t>(kListenerW | i));
					if (w < 0) break;
					_lst[i] = Listener();
					_lst[i].st = Listener::NeedBind;
					_lst[i].port = k.port;
					_lst[i].w = w;
					_lst[i].session = _wSession[w];
					l = i;
				}
			}
		}
		if (l < 0) { k.req = Sock::NoReq; socketFailed(s, SocketEvent::Failed); continue; }
		k.req = Sock::NoReq;
		k.mode = Sock::Listening;
		k.reported = false;
		_lst[l].idle = false;
		if (_lst[l].st == Listener::Open) { k.reported = true; emit(s, SocketEvent::Listening); }
	}
	// A listening socket nobody listens on: closed after a while.
	for (uint8_t l = 0; l < winc_listeners; ++l) {
		Listener &L = _lst[l];
		if (L.st == Listener::Free) continue;
		bool wanted = false;
		for (uint8_t s = 0; s < _nSocks; ++s) {
			if ((_sock[s].mode == Sock::Listening || _sock[s].req == Sock::ListenReq) && _sock[s].port == L.port) wanted = true;
		}
		if (wanted) { L.idle = false; continue; }
		if (!L.idle) { L.idle = true; L.idleSince = now; continue; }
		if (_txPending && (_txFor == TxFor::bind || _txFor == TxFor::listen) && _txIdx == static_cast<int8_t>(l)) continue;
		if (L.st == Listener::NeedBind || now - L.idleSince > winc_listen_linger_ms) {
			if (L.w >= 0) queueClose(L.w);
			L = Listener();
		}
	}
}

template <typename TTransport>
void winc1500<TTransport>::beginMessage(uint8_t gid, uint8_t op, uint16_t ctrlLen) {
	memset(_tx, 0, hif_header_bytes + ctrlLen);
	_tx[0] = gid;
	_tx[1] = static_cast<uint8_t>(op & ~req_data_pkt);
	_txLen = static_cast<uint16_t>(hif_header_bytes + ctrlLen);
	_txOp = op;
	_txGid = gid;
	_txPending = true;
}

// The next request for the module, built in _tx. false: none.
template <typename TTransport>
bool winc1500<TTransport>::prepareRequest(uint32_t now) {
	if (_txPending) return true;	// one the module had no buffer for: the same again
	uint8_t *c = _tx + hif_header_bytes;
	_txIdx = -1;

	// The set-up after start-up: DHCP or a static address, SNTP on.
	if (!_ready) {
		if (_setupStep == 0) {
			beginMessage(group_ip, _cfg.dhcp ? ip_req_enable_dhcp : ip_req_disable_dhcp, 0);
			_txFor = TxFor::setup;
			return true;
		}
		if (_setupStep == 1 && !_cfg.dhcp) {
			beginMessage(group_ip, ip_req_static_ip_conf, ip_config_bytes);
			memcpy(c + 0, _cfg.ip.b, 4);
			memcpy(c + 4, _cfg.gateway.b, 4);
			memcpy(c + 8, _cfg.dns.b, 4);
			memcpy(c + 12, _cfg.subnet.b, 4);
			_txFor = TxFor::setup;
			return true;
		}
		if (_setupStep <= 2) {
			_setupStep = 2;
			beginMessage(group_wifi, wifi_req_enable_sntp, 0);
			_txFor = TxFor::setup;
			return true;
		}
		return false;
	}

	if (_leaveReq && (_assoc || _joining) && !_leaving) {
		beginMessage(group_wifi, wifi_req_disconnect, 0);
		_txFor = TxFor::leave;
		return true;
	}
	if (_joinReq && !_leaveReq && !_leaving && !_assoc && !_joining && _ssid[0] != 0 &&
	    (!_rejoin || static_cast<int32_t>(now - _rejoinAt) >= 0)) {
		beginMessage(group_wifi, wifi_req_connect, connect_bytes);
		const size_t np = strlen(_pass);
		if (np) memcpy(c + connect_psk, _pass, np);
		c[connect_sec_type] = np ? sec_wpa_psk : sec_open;
		put16(c + connect_channel, channel_all);
		memcpy(c + connect_ssid, _ssid, strlen(_ssid));
		c[connect_no_save] = 1;	// don't write it to the module's flash every time
		_txFor = TxFor::join;
		return true;
	}
	if (_closeMask) {
		uint8_t w = 0;
		while (!(_closeMask & (1u << w))) ++w;
		beginMessage(group_ip, sock_cmd_close, close_bytes);
		c[0] = w;
		put16(c + 2, _wSession[w]);
		_txFor = TxFor::close;
		_txIdx = static_cast<int8_t>(w);
		return true;
	}
	if (!_up) return false;

	// Listening sockets: BIND, then LISTEN.
	for (uint8_t l = 0; l < winc_listeners; ++l) {
		Listener &L = _lst[l];
		if (L.st == Listener::NeedBind) {
			beginMessage(group_ip, sock_cmd_bind, bind_bytes);
			put16(c + 0, af_inet);
			c[2] = static_cast<uint8_t>(L.port >> 8);
			c[3] = static_cast<uint8_t>(L.port);
			c[8] = static_cast<uint8_t>(L.w);
			put16(c + 10, L.session);
			_txFor = TxFor::bind;
			_txIdx = static_cast<int8_t>(l);
			return true;
		}
		if (L.st == Listener::NeedListen) {
			beginMessage(group_ip, sock_cmd_listen, listen_bytes);
			c[0] = static_cast<uint8_t>(L.w);
			c[1] = 2;	// backlog
			put16(c + 2, L.session);
			_txFor = TxFor::listen;
			_txIdx = static_cast<int8_t>(l);
			return true;
		}
	}
	// Connects.
	for (uint8_t s = 0; s < _nSocks; ++s) {
		Sock &k = _sock[s];
		if (k.req != Sock::ConnectReq) continue;
		if (k.w < 0) {
			k.w = allocW(s);
			if (k.w < 0) { socketFailed(s, SocketEvent::Failed); continue; }	// all of the module's sockets in use
			k.session = _wSession[k.w];
		}
		beginMessage(group_ip, sock_cmd_connect, connect_cmd_bytes);
		put16(c + 0, af_inet);
		c[2] = static_cast<uint8_t>(k.port >> 8);
		c[3] = static_cast<uint8_t>(k.port);
		memcpy(c + 4, k.ip.b, 4);
		c[8] = static_cast<uint8_t>(k.w);
		put16(c + 10, k.session);
		_txFor = TxFor::connect;
		_txIdx = static_cast<int8_t>(s);
		return true;
	}
	// Data to send, round robin.
	for (uint8_t i = 0; i < _nSocks; ++i) {
		const uint8_t s = static_cast<uint8_t>((_sendRr + i) % _nSocks);
		Sock &k = _sock[s];
		if (k.mode != Sock::Established || k.sendInFlight || k.ending || _host == nullptr || _host->txPending(s) == 0) continue;
		const size_t n = _host->txTake(s, _tx + hif_header_bytes + send_data_offset, socket_max_send);
		if (n == 0) continue;
		beginMessage(group_ip, static_cast<uint8_t>(sock_cmd_send | req_data_pkt), send_cmd_bytes);
		c[0] = static_cast<uint8_t>(k.w);
		put16(c + 2, static_cast<uint16_t>(n));
		put16(c + 12, k.session);
		_txLen = static_cast<uint16_t>(hif_header_bytes + send_data_offset + n);
		k.sendLen = static_cast<uint16_t>(n);
		_txFor = TxFor::send;
		_txIdx = static_cast<int8_t>(s);
		_sendRr = static_cast<uint8_t>((s + 1) % _nSocks);
		return true;
	}
	// A receive outstanding on each connection that has room for it.
	for (uint8_t i = 0; i < _nSocks; ++i) {
		const uint8_t s = static_cast<uint8_t>((_recvRr + i) % _nSocks);
		Sock &k = _sock[s];
		if (k.mode != Sock::Established || k.recvArmed || k.ending || _host == nullptr) continue;
		if ((_rxOwner == static_cast<int8_t>(s) && _rxLen != 0) || _heldSock == static_cast<int8_t>(s)) continue;
		if (_host->rxSpace(s) == 0) continue;
		beginMessage(group_ip, sock_cmd_recv, recv_cmd_bytes);
		put32(c + 0, 0xFFFFFFFFu);	// no timeout
		c[4] = static_cast<uint8_t>(k.w);
		put16(c + 6, k.session);
		_txFor = TxFor::recv;
		_txIdx = static_cast<int8_t>(s);
		_recvRr = static_cast<uint8_t>((s + 1) % _nSocks);
		return true;
	}
	if (_dnsReq && !_dnsSent) {
		const uint16_t n = static_cast<uint16_t>(strlen(_dnsName) + 1);
		beginMessage(group_ip, sock_cmd_dns_resolve, n);
		memcpy(c, _dnsName, n);
		_txFor = TxFor::dns;
		return true;
	}
	if (_timeReq && !_timeSent && static_cast<int32_t>(now - _timeNext) >= 0) {
		beginMessage(group_wifi, wifi_req_get_sys_time, 0);
		_txFor = TxFor::time;
		return true;
	}
	if (static_cast<int32_t>(now - _nextRssi) >= 0) {
		beginMessage(group_wifi, wifi_req_current_rssi, 0);
		_txFor = TxFor::rssi;
		return true;
	}
	return false;
}

// The request in _tx reached the module.
template <typename TTransport>
void winc1500<TTransport>::onSent(uint32_t now) {
	_txPending = false;
	_stats.messagesOut++;
	switch (_txFor) {
	case TxFor::setup:
		_setupStep++;
		if (_setupStep == 1 && _cfg.dhcp) _setupStep = 2;
		if (_setupStep >= 3) {
			_ready = true;
			_nextRssi = now + _param.rssiPollMs;
			devEvent(DeviceEvent::Ready);
		}
		break;
	case TxFor::leave:
		_leaveReq = false;
		_leaving = true;
		_leaveAt = now;
		_joining = false;
		break;
	case TxFor::join:
		_joinReq = false;
		_joining = true;
		_joinAt = now;
		break;
	case TxFor::close:
		_closeMask = static_cast<uint8_t>(_closeMask & ~(1u << _txIdx));
		_wOwner[_txIdx] = kFreeW;
		break;
	case TxFor::bind:
		if (_lst[_txIdx].st == Listener::NeedBind) { _lst[_txIdx].st = Listener::BindSent; _lst[_txIdx].since = now; }
		break;
	case TxFor::listen:
		if (_lst[_txIdx].st == Listener::NeedListen) { _lst[_txIdx].st = Listener::ListenSent; _lst[_txIdx].since = now; }
		break;
	case TxFor::connect: {
		Sock &k = _sock[_txIdx];
		k.req = Sock::NoReq;
		k.mode = Sock::Connecting;
		k.sent = true;
		k.since = now;
		break;
	}
	case TxFor::send: {
		Sock &k = _sock[_txIdx];
		k.sendInFlight = true;
		k.since = now;
		break;
	}
	case TxFor::recv:
		_sock[_txIdx].recvArmed = true;
		break;
	case TxFor::dns:
		_dnsSent = true;
		_dnsAt = now;
		break;
	case TxFor::time:
		_timeSent = true;
		if (_timeAt == 0) _timeAt = now ? now : 1;
		break;
	case TxFor::rssi:
		_nextRssi = now + _param.rssiPollMs;
		break;
	default:
		break;
	}
	_txFor = TxFor::none;
}

// Sending a message: its header in reg_nmi_state, ask for a buffer, wait
// for one, write the message there, and say so.
template <typename TTransport>
uint32_t winc1500<TTransport>::stepSend(uint32_t now) {
	OpResult r;
	switch (_pstep) {
	case 0:
		if (!_opActive) {
			writeReg(reg_nmi_state, static_cast<uint32_t>(_txGid) | (static_cast<uint32_t>(_txOp) << 8) |
			                        (static_cast<uint32_t>(_txLen) << 16));
			put16(_tx + 2, _txLen);
			return 0;
		}
		break;
	case 1:
		if (!_opActive) { writeReg(reg_rcv_ctrl_2, 2); _allocAt = now; return 0; }
		break;
	case 2:
		if (!_opActive) { readReg(reg_rcv_ctrl_2); return 0; }
		break;
	case 3:
		if (!_opActive) { readReg(reg_rcv_ctrl_4); return 0; }
		break;
	case 4:
		if (!_opActive) { writeBlock(_txDma, _tx, _txLen); return 0; }
		break;
	case 5:
		if (!_opActive) { writeReg(reg_rcv_ctrl_3, (_txDma << 2) | 2u); return 0; }
		break;
	}
	r = runOp(now);
	if (r == OpResult::busy) return 0;
	if (r == OpResult::failed) { fail(winc_error_t::bus, now); return 0; }
	switch (_pstep) {
	case 2:
		if (_opValue & 2u) {	// no buffer yet
			if (now - _allocAt > winc_alloc_timeout_ms) {
				_stats.allocWaits++;
				if (++_allocFails >= winc_alloc_fails_max) { fail(winc_error_t::bus, now); return 0; }	// not taking requests: restarted behind our back?
				_sendBackoffUntil = now + winc_alloc_backoff_ms;
				_proc = Proc::none;	// the request stays in _tx, for later
				return 0;
			}
			return 0;	// ask again
		}
		_pstep = 3;
		return 0;
	case 3:
		_txDma = _opValue;
		if (_txDma == 0) {
			_stats.allocWaits++;
			if (++_allocFails >= winc_alloc_fails_max) { fail(winc_error_t::bus, now); return 0; }
			_sendBackoffUntil = now + winc_alloc_backoff_ms;
			_proc = Proc::none;
			return 0;
		}
		_pstep = 4;
		return 0;
	case 5:
		_proc = Proc::none;
		_allocFails = 0;
		onSent(now);
		return 0;
	default:
		_pstep++;
		return 0;
	}
}

// Checking for a message from the module, and reading it.
template <typename TTransport>
uint32_t winc1500<TTransport>::stepReceive(uint32_t now) {
	switch (_pstep) {
	case 0:
		if (!_opActive) { readReg(reg_rcv_ctrl_0); return 0; }
		break;
	case 1:
		if (!_opActive) { writeReg(reg_rcv_ctrl_0, _ctrl0 & ~1u); return 0; }
		break;
	case 2:
		if (!_opActive) { readReg(reg_rcv_ctrl_1); return 0; }
		break;
	case 3:
		if (!_opActive) {
			const uint16_t n = _msgSize < sizeof _hdr ? _msgSize : static_cast<uint16_t>(sizeof _hdr);
			readBlock(_msgAddr, _hdr, n);
			return 0;
		}
		break;
	}
	const OpResult r = runOp(now);
	if (r == OpResult::busy) return 0;
	if (r == OpResult::failed) { fail(winc_error_t::bus, now); return 0; }
	switch (_pstep) {
	case 0:
		_ctrl0 = _opValue;
		if (!(_ctrl0 & 1u)) {	// nothing
			_proc = Proc::none;
			_nextCheck = now + _param.pollMs;
			return 0;
		}
		_msgSize = static_cast<uint16_t>((_ctrl0 >> 2) & 0xFFF);
		_pstep = 1;
		return 0;
	case 1:
		_msgHeld = true;
		if (_msgSize < hif_header_bytes) {	// a broken message: let it go
			_proc = Proc::rx_done;
			_pstep = 0;
			return 0;
		}
		_pstep = 2;
		return 0;
	case 2:
		_msgAddr = _opValue;
		_pstep = 3;
		return 0;
	default:
		_stats.messagesIn++;
		_proc = Proc::none;
		dispatch(now);
		if (_heldLeft == 0) { _proc = Proc::rx_done; _pstep = 0; }	// otherwise: its data first
		return 0;
	}
}

// A RECV reply's data, a buffer at a time; then "RX done".
template <typename TTransport>
uint32_t winc1500<TTransport>::stepHeldData(uint32_t now) {
	if (!_opActive) {
		if (_heldSock < 0) {	// nobody wants it any more
			_heldLeft = 0;
			_proc = Proc::rx_done;
			_pstep = 0;
			return 0;
		}
		_heldPiece = _heldLeft < winc_rx_bytes ? _heldLeft : winc_rx_bytes;
		readBlock(_heldAddr, _rx, _heldPiece);
		return 0;
	}
	const OpResult r = runOp(now);
	if (r == OpResult::busy) return 0;
	if (r == OpResult::failed) { fail(winc_error_t::bus, now); return 0; }
	if (_heldSock >= 0) {
		_rxLen = _heldPiece;
		_rxPos = 0;
		_rxOwner = _heldSock;
	}
	_heldAddr += _heldPiece;
	_heldLeft = static_cast<uint16_t>(_heldLeft - _heldPiece);
	if (_heldLeft == 0) {
		_heldSock = -1;
		_proc = Proc::rx_done;
		_pstep = 0;
	} else {
		_proc = Proc::none;	// the rest when this has gone to the reader
	}
	deliverRx();
	return 0;
}

template <typename TTransport>
uint32_t winc1500<TTransport>::stepRxDone(uint32_t now) {
	if (!_opActive) {
		if (_pstep == 0) readReg(reg_rcv_ctrl_0);
		else writeReg(reg_rcv_ctrl_0, _ctrl0 | 2u);
		return 0;
	}
	const OpResult r = runOp(now);
	if (r == OpResult::busy) return 0;
	if (r == OpResult::failed) { fail(winc_error_t::bus, now); return 0; }
	if (_pstep == 0) { _ctrl0 = _opValue; _pstep = 1; return 0; }
	_msgHeld = false;
	_heldCounted = false;
	_proc = Proc::none;
	_irq = true;	// another may be waiting
	return 0;
}

template <typename TTransport>
uint32_t winc1500<TTransport>::runMain(uint32_t now) {
	checkTimers(now);
	if (_state != winc_state_t::run) return 0;	// a timer failed the module
	deliverRx();
	progress(now);
	switch (_proc) {
	case Proc::receive: return stepReceive(now);
	case Proc::send: return stepSend(now);
	case Proc::held_data: return stepHeldData(now);
	case Proc::rx_done: return stepRxDone(now);
	default: break;
	}
	// A reply's data still in the module: when the buffer is free.
	if (_msgHeld && _heldLeft > 0) {
		if (_rxLen == 0 || _heldSock < 0) {
			_proc = Proc::held_data;
			_opActive = false;
			return 0;
		}
		if (!_heldCounted) { _heldCounted = true; _stats.heldReplies++; }
	}
	// News from the module.
	if (!_msgHeld && (_irq || static_cast<int32_t>(now - _nextCheck) >= 0)) {
		_irq = false;
		_proc = Proc::receive;
		_pstep = 0;
		_opActive = false;
		return 0;
	}
	// Requests.
	if (static_cast<int32_t>(now - _sendBackoffUntil) >= 0 && prepareRequest(now)) {
		_proc = Proc::send;
		_pstep = 0;
		_opActive = false;
		return 0;
	}
	return nextWake(now);
}

template <typename TTransport>
uint32_t winc1500<TTransport>::nextWake(uint32_t now) {
	uint32_t w = 1000;
	auto until = [&](uint32_t t) { const int32_t d = static_cast<int32_t>(t - now); return d <= 0 ? 0u : static_cast<uint32_t>(d); };
	if (!_msgHeld) { const uint32_t d = until(_nextCheck); if (d < w) w = d; }
	else if (_param.pollMs < w) w = _param.pollMs;	// a reader to wait for: the host calls sooner when room appears
	if (_txPending) { const uint32_t d = until(_sendBackoffUntil); if (d < w) w = d; }
	if (_up) {
		uint32_t d = until(_nextRssi); if (d < w) w = d;
		if (_timeReq && !_timeSent) { d = until(_timeNext); if (d < w) w = d; }
	}
	bool timing = _joining || (_dnsReq && _dnsSent) || (_timeReq && _timeAt != 0);
	for (uint8_t s = 0; s < _nSocks; ++s) {
		if (_sock[s].mode == Sock::Connecting || _sock[s].sendInFlight) timing = true;
	}
	for (uint8_t l = 0; l < winc_listeners; ++l) if (_lst[l].st != Listener::Free) timing = true;
	if (timing && w > 100) w = 100;
	return w;
}

// ---------------------------------------------------------------------
// Messages from the module
// ---------------------------------------------------------------------

template <typename TTransport>
void winc1500<TTransport>::dispatch(uint32_t now) {
	const uint8_t gid = _hdr[0];
	const uint8_t op = _hdr[1];
	const uint16_t len = le16(_hdr + 2);
	if (len < hif_header_bytes || (len != _msgSize && (len > _msgSize || _msgSize - len > 4))) return;	// corrupted: dropped
	const uint16_t got = _msgSize < sizeof _hdr ? _msgSize : static_cast<uint16_t>(sizeof _hdr);
	const uint16_t n = static_cast<uint16_t>((len < got ? len : got) - hif_header_bytes);
	const uint8_t *p = _hdr + hif_header_bytes;
	if (gid == group_wifi) wifiMessage(op, p, n, now);
	else if (gid == group_ip) ipMessage(op, p, n, now);
}

template <typename TTransport>
void winc1500<TTransport>::wifiMessage(uint8_t op, const uint8_t *p, uint16_t n, uint32_t now) {
	switch (op) {
	case wifi_resp_con_state: {
		if (n < 2) return;
		if (p[0] == wifi_connected) {
			_assoc = true;
			_joinAt = now;	// from here, the time for an address
			if (!_cfg.dhcp) {
				_net = _cfg;
				_up = true;
				_joining = false;
				_rejoin = false;
				if (_host) _host->addressChanged(_net);
				devEvent(DeviceEvent::LinkUp);
				_nextRssi = now;
			}
			return;
		}
		const bool wasUp = _up, wasJoining = _joining, wasLeaving = _leaving;
		_assoc = false;
		_joining = false;
		_leaving = false;
		if (wasUp) {
			linkLost(now);
			if (!wasLeaving && _ssid[0] != 0) {	// the network went away: try it again in a while
				_rejoin = true;
				_joinReq = true;
				_rejoinAt = now + winc_rejoin_ms;
			}
		} else if (wasJoining && !wasLeaving) {
			_joinErr = p[1];
			if (_rejoin) { _joinReq = true; _rejoinAt = now + winc_rejoin_ms; }
			else devEvent(DeviceEvent::JoinFailed);
		} else if (wasLeaving && !_leaveQuiet) {
			devEvent(DeviceEvent::LinkDown);
		}
		return;
	}
	case wifi_req_dhcp_conf: {
		if (n < 16) return;
		NetConfig c = _cfg;
		c.dhcp = true;
		memcpy(c.ip.b, p + 0, 4);
		memcpy(c.gateway.b, p + 4, 4);
		memcpy(c.dns.b, p + 8, 4);
		memcpy(c.subnet.b, p + 12, 4);
		if (!_assoc) return;
		if (_up) {
			if (c.ip == _net.ip) { _net = c; return; }	// renewed, same address
			dropSockets(now);	// a new address: the connections are gone
		}
		_net = c;
		_up = true;
		_joining = false;
		_joinErr = 0;
		_rejoin = false;
		if (_host) _host->addressChanged(_net);
		devEvent(DeviceEvent::LinkUp);
		_nextRssi = now;
		return;
	}
	case wifi_resp_current_rssi:
		if (n >= 1) _rssi = static_cast<int8_t>(p[0]);
		return;
	case wifi_resp_get_sys_time:
		if (n >= sys_time_bytes) timeReply(p, now);
		return;
	default:
		return;
	}
}

// Days since 1970-01-01 (Howard Hinnant's days_from_civil).
static inline int64_t wincDaysFromCivil(int y, unsigned m, unsigned d) {
	y -= m <= 2;
	const int era = (y >= 0 ? y : y - 399) / 400;
	const unsigned yoe = static_cast<unsigned>(y - era * 400);
	const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return static_cast<int64_t>(era) * 146097 + static_cast<int64_t>(doe) - 719468;
}

template <typename TTransport>
void winc1500<TTransport>::timeReply(const uint8_t *p, uint32_t now) {
	const unsigned year = le16(p), mon = p[2], day = p[3], h = p[4], mi = p[5], se = p[6];
	_timeSent = false;
	if (!_timeReq) return;
	if (year < 2000 || mon < 1 || mon > 12 || day < 1 || day > 31 || h > 23 || mi > 59 || se > 60) {
		_timeNext = now + winc_sntp_poll_ms;	// not synchronised yet: ask again
		return;
	}
	const int64_t secs = wincDaysFromCivil(static_cast<int>(year), mon, day) * 86400 + h * 3600 + mi * 60 + se;
	_timeReq = false;
	// The module has whole seconds: the middle of that second.
	if (_host) _host->timeReceived(true, static_cast<uint64_t>(secs) * 1000u + 500u, now);
}

template <typename TTransport>
void winc1500<TTransport>::ipMessage(uint8_t op, const uint8_t *p, uint16_t n, uint32_t now) {
	(void)now;
	switch (op) {
	case sock_cmd_bind:
	case sock_cmd_listen: {
		if (n < bind_reply_bytes) return;
		const int8_t w = static_cast<int8_t>(p[0]);
		const int8_t status = static_cast<int8_t>(p[1]);
		for (uint8_t l = 0; l < winc_listeners; ++l) {
			Listener &L = _lst[l];
			if (L.w != w) continue;
			if (op == sock_cmd_bind && L.st == Listener::BindSent) {
				if (status == sock_err_none) L.st = Listener::NeedListen;
				else listenerFailed(l);
			} else if (op == sock_cmd_listen && L.st == Listener::ListenSent) {
				if (status != sock_err_none) { listenerFailed(l); return; }
				L.st = Listener::Open;
				for (uint8_t s = 0; s < _nSocks; ++s) {
					Sock &k = _sock[s];
					if (k.mode == Sock::Listening && k.port == L.port && !k.reported) {
						k.reported = true;
						emit(s, SocketEvent::Listening);
					}
				}
			}
			return;
		}
		return;
	}
	case sock_cmd_accept: {
		if (n < accept_reply_bytes) return;
		const int8_t lw = static_cast<int8_t>(p[8]);
		const int8_t cw = static_cast<int8_t>(p[9]);
		if (cw < 0 || cw >= static_cast<int8_t>(tcp_sockets)) return;
		int l = -1;
		for (uint8_t i = 0; i < winc_listeners; ++i) if (_lst[i].st == Listener::Open && _lst[i].w == lw) l = i;
		if (_wOwner[cw] != kFreeW && _wOwner[cw] != kStrayW && !(_wOwner[cw] & kListenerW)) {
			// We'd numbered a socket of ours the same: it can't be ours any more.
			socketFailed(_wOwner[cw], SocketEvent::Failed);
		}
		_closeMask = static_cast<uint8_t>(_closeMask & ~(1u << cw));
		if (l >= 0) {
			for (uint8_t s = 0; s < _nSocks; ++s) {
				Sock &k = _sock[s];
				if (k.mode != Sock::Listening || k.port != _lst[l].port || !k.reported) continue;
				k.mode = Sock::Established;
				k.w = cw;
				k.session = nextSession();
				k.ip = IpAddress(p[4], p[5], p[6], p[7]);
				_wOwner[cw] = s;
				_wSession[cw] = k.session;
				emit(s, SocketEvent::Connected);
				return;
			}
		}
		_wSession[cw] = nextSession();
		queueClose(cw);	// nobody listening: turn the peer away
		return;
	}
	case sock_cmd_connect: {
		if (n < connect_reply_bytes) return;
		const int8_t w = static_cast<int8_t>(p[0]);
		const int8_t err = static_cast<int8_t>(p[1]);
		for (uint8_t s = 0; s < _nSocks; ++s) {
			Sock &k = _sock[s];
			if (k.mode != Sock::Connecting || k.w != w || !k.sent) continue;
			if (err == sock_err_none) {
				k.mode = Sock::Established;
				k.sent = false;
				emit(s, SocketEvent::Connected);
			} else {
				socketFailed(s, SocketEvent::Failed);
			}
			return;
		}
		return;
	}
	case sock_cmd_send: {
		if (n < send_reply_bytes) return;
		const int8_t w = static_cast<int8_t>(p[0]);
		const int16_t sent = static_cast<int16_t>(le16(p + 2));
		const uint16_t session = le16(p + 4);
		for (uint8_t s = 0; s < _nSocks; ++s) {
			Sock &k = _sock[s];
			if (k.mode != Sock::Established || k.w != w || k.session != session || !k.sendInFlight) continue;
			k.sendInFlight = false;
			if (sent <= 0) { socketFailed(s, SocketEvent::Failed); return; }
			_stats.txBytes += k.sendLen;
			return;
		}
		return;
	}
	case sock_cmd_recv: {
		if (n < recv_reply_bytes) return;
		const int16_t status = static_cast<int16_t>(le16(p + 8));
		const uint16_t offset = le16(p + 10);
		const int8_t w = static_cast<int8_t>(p[12]);
		const uint16_t session = le16(p + 14);
		for (uint8_t s = 0; s < _nSocks; ++s) {
			Sock &k = _sock[s];
			if (k.mode != Sock::Established || k.w != w || k.session != session) continue;
			k.recvArmed = false;
			if (status > 0) {
				// The data, at offset from the reply's start, is read once
				// the buffer is free; the module waits until then.
				_heldSock = static_cast<int8_t>(s);
				_heldAddr = _msgAddr + hif_header_bytes + offset;
				_heldLeft = static_cast<uint16_t>(status);
				return;
			}
			if (status == sock_err_timeout) return;	// (no timeout is asked for) ask again
			// Closed by the peer (0, or "aborted"), or an error: after the
			// reader has had what's buffered.
			k.ending = true;
			k.endEv = (status == 0 || status == sock_err_conn_aborted) ? SocketEvent::Closed : SocketEvent::Failed;
			return;
		}
		return;
	}
	case sock_cmd_dns_resolve: {
		if (n < dns_reply_bytes || !_dnsReq || !_dnsSent) return;
		if (strncmp(reinterpret_cast<const char *>(p), _dnsName, hostname_max) != 0) return;	// an older question's answer
		const IpAddress ip(p[64], p[65], p[66], p[67]);
		_dnsReq = _dnsSent = false;
		if (_host) _host->resolved(!ip.isZero(), ip);
		return;
	}
	default:
		return;
	}
}

}  // namespace WINC1500
