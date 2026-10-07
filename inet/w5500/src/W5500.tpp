/*
 * W5500.tpp
 *
 *  w5500's member definitions. W5500.h includes this file at its end,
 *  since templates have to be visible wherever they are used: include
 *  W5500.h, not this file.
 */

#ifndef W5500_TPP_
#define W5500_TPP_

#include "../inc/W5500.h"

namespace W5500 {

typedef w5500_state_t St;

template <typename TTransport>
template <typename... TArgs>
w5500<TTransport>::w5500(const w5500_param_t &param, TArgs&&... transportArgs)
	: TTransport(std::forward<TArgs>(transportArgs)...), _param(param) {}

// ---- requests, all from the driver thread ----

template <typename TTransport>
void w5500<TTransport>::configure(const NetConfig &cfg) {
	_cfg = cfg;
	_configured = true;
	_reconfigure = true;	// picked up at the next idle (or from unconfigured/error)
}

template <typename TTransport>
bool w5500<TTransport>::connect(uint8_t s, const IpAddress &ip, uint16_t port, uint16_t localPort) {
	if (s >= w5500_sockets || !_configured) return false;
	Sock &k = _sock[s];
	k.req = Sock::ReqConnect;
	k.reqIp = ip;
	k.reqPort = port;
	k.reqLocalPort = localPort;
	return true;
}

template <typename TTransport>
bool w5500<TTransport>::listen(uint8_t s, uint16_t port) {
	if (s >= w5500_sockets || !_configured) return false;
	Sock &k = _sock[s];
	k.req = Sock::ReqListen;
	k.reqIp = IpAddress();
	k.reqPort = 0;
	k.reqLocalPort = port;
	return true;
}

template <typename TTransport>
void w5500<TTransport>::close(uint8_t s) {
	if (s >= w5500_sockets) return;
	if (!_ready) {
		// Not running: every socket is already closed. Answer now, or
		// whoever is waiting for Closed waits until the chip comes back.
		_sock[s].req = Sock::NoReq;
		emit(s, SocketEvent::Closed);
		return;
	}
	_sock[s].req = Sock::ReqClose;
}

template <typename TTransport>
uint16_t w5500<TTransport>::speedMbps() const {
	if (!_link) return 0;
	return (_phy & w5500_PHY_SPD) ? 100 : 10;
}

template <typename TTransport>
bool w5500<TTransport>::fullDuplex() const {
	return _link && (_phy & w5500_PHY_DPX);
}

// ---- the state machine ----

template <typename TTransport>
uint32_t w5500<TTransport>::poll(uint32_t nowMs) {
	// Run states back to back until one has to wait. Bounded, so the
	// driver thread gets back to its queue now and then even when
	// there is always more to do.
	for (int i = 0; i < 64; ++i) {
		const uint32_t wait = step(nowMs);
		if (wait != 0) return wait;
	}
	return 0;
}

template <typename TTransport>
uint32_t w5500<TTransport>::xfer(bool write, uint8_t bsb, uint16_t addr, uint8_t *buf, uint16_t len,
                                 St then, uint32_t nowMs) {
	_xWrite = write;
	_xBsb = bsb;
	_xAddr = addr;
	_xBuf = buf;
	_xLen = len;
	_xThen = then;
	enter(St::xfer_issue, nowMs);
	return 0;
}

template <typename TTransport>
uint32_t w5500<TTransport>::command(uint8_t s, uint8_t cmd, St then, uint32_t nowMs) {
	_cmdSock = s;
	_cmdByte = cmd;
	_cmdThen = then;
	_cmdTries = 0;
	return xfer(true, w5500_bsb_sock_reg(s), w5500_Sn_CR, &_cmdByte, 1, St::cmd_poll, nowMs);
}

template <typename TTransport>
uint32_t w5500<TTransport>::readSir(uint32_t nowMs) {
	_lastSir = nowMs;
	return xfer(false, w5500_bsb_common(), w5500_SIR, &_sir, 1, St::check_sir, nowMs);
}

template <typename TTransport>
uint32_t w5500<TTransport>::service(uint8_t s, bool fromSir, uint32_t nowMs) {
	_cur = s;
	_svcFromSir = fromSir;
	_sock[s].lastSvc = nowMs;
	return xfer(false, w5500_bsb_sock_reg(s), w5500_Sn_IR, _svc, 2, St::svc_check, nowMs);
}

template <typename TTransport>
uint32_t w5500<TTransport>::rxBegin(uint8_t s, uint32_t nowMs) {
	_cur = s;
	_tries = 0;
	return xfer(false, w5500_bsb_sock_reg(s), w5500_Sn_RX_RSR, _a, 4, St::rx_again, nowMs);
}

template <typename TTransport>
uint32_t w5500<TTransport>::txBegin(uint8_t s, uint32_t nowMs) {
	_cur = s;
	_tries = 0;
	return xfer(false, w5500_bsb_sock_reg(s), w5500_Sn_TX_FSR, _a, 6, St::tx_again, nowMs);
}

template <typename TTransport>
uint32_t w5500<TTransport>::step(uint32_t nowMs) {
	Sock &k = _sock[_cur];

	switch (_state) {
	case St::unconfigured:
		if (_reconfigure) {
			enter(St::reset, nowMs);
			return 0;
		}
		return 1000;

	// ---- one register access ----
	case St::xfer_issue: {
		const uint8_t hdr[3] = {
			static_cast<uint8_t>(_xAddr >> 8), static_cast<uint8_t>(_xAddr),
			static_cast<uint8_t>((_xBsb << 3) | (_xWrite ? w5500_ctl_write : 0)) };
		const bool started = _xWrite ? this->beginWrite(hdr, 3, _xBuf, _xLen)
		                             : this->beginRead(hdr, 3, _xBuf, _xLen);
		if (started) {
			enter(St::xfer_wait, nowMs);
			return 0;
		}
		// Bus in use by another device on it: try again shortly.
		if (elapsed(nowMs, w5500_bus_timeout_ms)) return fail(nowMs);
		return 1;
	}
	case St::xfer_wait:
		// Under FreeRtosTransport this sleeps until the SPI interrupt.
		if (this->isBusy()) {
			if (elapsed(nowMs, w5500_bus_timeout_ms)) return fail(nowMs);
			return 0;
		}
		if (this->lastOpFailed()) return fail(nowMs);
		enter(_xThen, nowMs);
		return 0;

	// ---- Sn_CR ----
	case St::cmd_poll:
		return xfer(false, w5500_bsb_sock_reg(_cmdSock), w5500_Sn_CR, &_cmdRead, 1, St::cmd_check, nowMs);
	case St::cmd_check:
		if (_cmdRead == 0) {
			enter(_cmdThen, nowMs);
			return 0;
		}
		if (++_cmdTries > 50) return fail(nowMs);	// the chip never took it
		enter(St::cmd_poll, nowMs);
		return 0;

	// ---- start up ----
	case St::reset:
		_reconfigure = false;	// what configure() asked for is being done now
		if (!bufConfigValid()) return fail(nowMs);
		_phaseStart = nowMs;
		_w[0] = w5500_MR_RST;
		return xfer(true, w5500_bsb_common(), w5500_MR, _w, 1, St::reset_poll, nowMs);
	case St::reset_poll:
		return xfer(false, w5500_bsb_common(), w5500_MR, &_rd, 1, St::reset_check, nowMs);
	case St::reset_check:
		if (_rd & w5500_MR_RST) {
			if (after(nowMs, _phaseStart, w5500_reset_timeout_ms)) return fail(nowMs);
			enter(St::reset_poll, nowMs);
			return 1;
		}
		enter(St::read_version, nowMs);
		return 0;
	case St::read_version:
		return xfer(false, w5500_bsb_common(), w5500_VERSIONR, &_rd, 1, St::check_version, nowMs);
	case St::check_version:
		if (_rd != w5500_version) return fail(nowMs);	// not a W5500, or not answering (0x00/0xFF)
		enter(St::write_addr, nowMs);
		return 0;
	case St::write_addr:
		for (int i = 0; i < 4; ++i) _w[i] = _cfg.gateway.b[i];
		for (int i = 0; i < 4; ++i) _w[4 + i] = _cfg.subnet.b[i];
		for (int i = 0; i < 6; ++i) _w[8 + i] = _cfg.mac.b[i];
		for (int i = 0; i < 4; ++i) _w[14 + i] = _cfg.ip.b[i];
		return xfer(true, w5500_bsb_common(), w5500_GAR, _w, 18, St::write_retry, nowMs);
	case St::write_retry:
		putBe16(_w, _param.retryTime100us);
		_w[2] = _param.retryCount;
		_bufIdx = 0;
		return xfer(true, w5500_bsb_common(), w5500_RTR, _w, 3, St::write_bufsize, nowMs);
	case St::write_bufsize:
		_w[0] = _param.rxBufKb[_bufIdx];
		_w[1] = _param.txBufKb[_bufIdx];
		return xfer(true, w5500_bsb_sock_reg(_bufIdx), w5500_Sn_RXBUF_SIZE, _w, 2, St::bufsize_next, nowMs);
	case St::bufsize_next:
		enter(++_bufIdx < w5500_sockets ? St::write_bufsize : St::write_simr, nowMs);
		return 0;
	case St::write_simr:
		_w[0] = 0xFF;	// every socket may raise INT
		return xfer(true, w5500_bsb_common(), w5500_SIMR, _w, 1, St::init_phy, nowMs);
	case St::init_phy:
		return xfer(false, w5500_bsb_common(), w5500_PHYCFGR, &_phy, 1, St::init_done, nowMs);
	case St::init_done:
		_ready = true;
		_lastSir = _lastPhy = nowMs;
		emitDev(DeviceEvent::Ready);
		applyPhy();
		enter(St::idle, nowMs);
		return 0;

	case St::idle:
		return schedule(nowMs);

	case St::read_phy:
		_lastPhy = nowMs;
		return xfer(false, w5500_bsb_common(), w5500_PHYCFGR, &_phy, 1, St::check_phy, nowMs);
	case St::check_phy:
		applyPhy();
		enter(St::idle, nowMs);
		return 0;

	// ---- which sockets have news ----
	case St::check_sir:
		_sirMask = _sir;
		if (_sirMask == 0) {
			enter(St::idle, nowMs);
			return 0;
		}
		for (uint8_t s = 0; s < w5500_sockets; ++s) {
			if (_sirMask & (1u << s)) return service(s, true, nowMs);
		}
		enter(St::idle, nowMs);
		return 0;

	// ---- one socket's Sn_IR/Sn_SR ----
	case St::svc_check:
		if (_svc[0] != 0) {
			_w[0] = _svc[0];	// writing a 1 clears that bit
			return xfer(true, w5500_bsb_sock_reg(_cur), w5500_Sn_IR, _w, 1, St::svc_eval, nowMs);
		}
		enter(St::svc_eval, nowMs);
		return 0;
	case St::svc_eval:
		return evaluate(_cur, _svc[0], _svc[1], nowMs);
	case St::svc_next:
		if (_svcFromSir) {
			_sirMask = static_cast<uint8_t>(_sirMask & ~(1u << _cur));
			for (uint8_t s = 0; s < w5500_sockets; ++s) {
				if (_sirMask & (1u << s)) return service(s, true, nowMs);
			}
			// More may have arrived while these were handled, with INT
			// held low the whole time and so no new edge. Look again
			// once there is nothing more pressing.
			_sirRecheck = true;
		}
		enter(St::idle, nowMs);
		return 0;

	// ---- connect / listen ----
	case St::open_mode:
		_w[0] = w5500_Sn_MR_TCP | w5500_Sn_MR_ND;
		return xfer(true, w5500_bsb_sock_reg(_cur), w5500_Sn_MR, _w, 1, St::open_ir, nowMs);
	case St::open_ir:
		_w[0] = 0xFF;
		return xfer(true, w5500_bsb_sock_reg(_cur), w5500_Sn_IR, _w, 1, St::open_addr, nowMs);
	case St::open_addr:
		// Sn_PORT, Sn_DHAR (unused by TCP), Sn_DIPR, Sn_DPORT: 14 bytes
		putBe16(_w, k.reqLocalPort);
		for (int i = 0; i < 6; ++i) _w[2 + i] = 0;
		for (int i = 0; i < 4; ++i) _w[8 + i] = k.reqIp.b[i];
		putBe16(_w + 12, k.reqPort);
		return xfer(true, w5500_bsb_sock_reg(_cur), w5500_Sn_PORT, _w, 14, St::open_cmd, nowMs);
	case St::open_cmd:
		return command(_cur, w5500_CR_OPEN, St::open_read_sr, nowMs);
	case St::open_read_sr:
		return xfer(false, w5500_bsb_sock_reg(_cur), w5500_Sn_SR, &_svc[1], 1, St::open_check, nowMs);
	case St::open_check:
		if (_svc[1] != w5500_SOCK_INIT) {
			k.mode = Sock::Closed;
			emit(_cur, SocketEvent::Failed);
			return command(_cur, w5500_CR_CLOSE, St::idle, nowMs);
		}
		return command(_cur, k.opening_listen ? w5500_CR_LISTEN : w5500_CR_CONNECT, St::open_done, nowMs);
	case St::open_done:
		k.mode = k.opening_listen ? Sock::Listening : Sock::Connecting;
		k.since = k.lastSvc = nowMs;
		if (k.opening_listen) emit(_cur, SocketEvent::Listening);
		enter(St::idle, nowMs);
		return 0;

	// ---- receive: chip -> host ----
	case St::rx_again:
		return xfer(false, w5500_bsb_sock_reg(_cur), w5500_Sn_RX_RSR, _b, 2, St::rx_check, nowMs);
	case St::rx_check: {
		// Sn_RX_RSR can change under us; the datasheet says to read it
		// until two reads agree.
		const uint16_t rsr = be16(_b);
		if (be16(_a) != rsr) {
			if (++_tries > 8) return fail(nowMs);
			return xfer(false, w5500_bsb_sock_reg(_cur), w5500_Sn_RX_RSR, _a, 4, St::rx_again, nowMs);
		}
		if (rsr == 0) {
			k.rxPending = false;
			enter(St::idle, nowMs);
			return 0;
		}
		size_t n = _host ? _host->rxSpace(_cur) : 0;
		if (n == 0) {
			// Host is full. rxPending stays set; the host wakes us when
			// a reader makes room.
			enter(St::idle, nowMs);
			return 0;
		}
		if (n > rsr) n = rsr;
		if (n > w5500_chunk_bytes) n = w5500_chunk_bytes;
		_moved = static_cast<uint16_t>(n);
		_ptr = be16(_a + 2);	// Sn_RX_RD. The chip wraps it within the socket's buffer.
		return xfer(false, w5500_bsb_sock_rx(_cur), _ptr, _chunk, _moved, St::rx_deliver, nowMs);
	}
	case St::rx_deliver:
		if (_host) _host->rxDeliver(_cur, _chunk, _moved);
		_ptr = static_cast<uint16_t>(_ptr + _moved);
		putBe16(_w, _ptr);
		return xfer(true, w5500_bsb_sock_reg(_cur), w5500_Sn_RX_RD, _w, 2, St::rx_recv, nowMs);
	case St::rx_recv:
		// rxPending stays set: idle comes back for the rest, taking
		// turns with the other sockets.
		return command(_cur, w5500_CR_RECV, St::idle, nowMs);

	// ---- send: host -> chip ----
	case St::tx_again:
		return xfer(false, w5500_bsb_sock_reg(_cur), w5500_Sn_TX_FSR, _b, 2, St::tx_check, nowMs);
	case St::tx_check:
		if (be16(_a) != be16(_b)) {
			if (++_tries > 8) return fail(nowMs);
			return xfer(false, w5500_bsb_sock_reg(_cur), w5500_Sn_TX_FSR, _a, 6, St::tx_again, nowMs);
		}
		_fsr = be16(_b);
		_ptr = be16(_a + 4);	// Sn_TX_WR
		_txWrote = 0;
		if (_fsr == 0) {
			// Chip's buffer is full of unacknowledged data. Try again
			// later rather than read FSR in a tight loop.
			k.txBlocked = true;
			k.txBlockedAt = nowMs;
			enter(St::idle, nowMs);
			return 0;
		}
		enter(St::tx_fill, nowMs);
		return 0;
	case St::tx_fill: {
		size_t n = _host ? _host->txPending(_cur) : 0;
		if (n > _fsr) n = _fsr;
		if (n > w5500_chunk_bytes) n = w5500_chunk_bytes;
		if (n != 0) n = _host->txTake(_cur, _chunk, n);
		if (n != 0) {
			_moved = static_cast<uint16_t>(n);
			return xfer(true, w5500_bsb_sock_tx(_cur), _ptr, _chunk, _moved, St::tx_wrote, nowMs);
		}
		if (_txWrote == 0) {
			enter(St::idle, nowMs);
			return 0;
		}
		putBe16(_w, _ptr);
		return xfer(true, w5500_bsb_sock_reg(_cur), w5500_Sn_TX_WR, _w, 2, St::tx_send, nowMs);
	}
	case St::tx_wrote:
		_ptr = static_cast<uint16_t>(_ptr + _moved);
		_fsr = static_cast<uint16_t>(_fsr - _moved);
		_txWrote = static_cast<uint16_t>(_txWrote + _moved);
		enter(St::tx_fill, nowMs);	// as much as fits, then one SEND
		return 0;
	case St::tx_send:
		return command(_cur, w5500_CR_SEND, St::tx_sent, nowMs);
	case St::tx_sent:
		k.sendInFlight = true;
		k.sendSince = k.lastSvc = nowMs;
		enter(St::idle, nowMs);
		return 0;

	case St::error:
		// A transfer given up on may still be in flight; it has to land
		// before the bus is free for the next.
		if (this->isBusy()) return 1;
		if (!elapsed(nowMs, w5500_error_backoff_ms)) return remaining(nowMs, _since, w5500_error_backoff_ms);
		enter(_configured ? St::reset : St::unconfigured, nowMs);
		return 0;
	}
	return fail(nowMs);	// unreachable
}

// What to do next, most urgent first. Returns 0 having started
// something, or how long nothing needs doing.
template <typename TTransport>
uint32_t w5500<TTransport>::schedule(uint32_t nowMs) {
	if (_reconfigure) {
		dropAll(SocketEvent::Failed);
		_ready = false;
		if (_link) {
			_link = false;
			emitDev(DeviceEvent::LinkDown);
		}
		enter(St::reset, nowMs);
		return 0;
	}

	// 1. What the user asked for.
	for (uint8_t s = 0; s < w5500_sockets; ++s) {
		if (_sock[s].req != Sock::NoReq) return startRequest(s, nowMs);
	}

	// 2. The INT pin.
	if (_irq) {
		_irq = false;
		return readSir(nowMs);
	}

	// 3. Sockets part way through something the chip raises no
	//    interrupt for, and our own backstop timeouts.
	for (uint8_t s = 0; s < w5500_sockets; ++s) {
		Sock &k = _sock[s];
		const bool busy = k.mode == Sock::Connecting || k.mode == Sock::Closing || k.sendInFlight;
		if (!busy) continue;
		if (k.mode == Sock::Connecting && after(nowMs, k.since, w5500_connect_timeout_ms)) {
			k = Sock();
			emit(s, SocketEvent::Failed);
			return command(s, w5500_CR_CLOSE, St::idle, nowMs);
		}
		if (k.mode == Sock::Closing && after(nowMs, k.since, w5500_close_timeout_ms)) {
			k = Sock();
			emit(s, SocketEvent::Closed);
			return command(s, w5500_CR_CLOSE, St::idle, nowMs);
		}
		if (k.sendInFlight && after(nowMs, k.sendSince, w5500_send_timeout_ms)) {
			k = Sock();
			emit(s, SocketEvent::Failed);
			return command(s, w5500_CR_CLOSE, St::idle, nowMs);
		}
		if (after(nowMs, k.lastSvc, _param.busyPollMs)) return service(s, false, nowMs);
	}

	// 4. Received data, if the host has room. Round robin so one busy
	//    socket can't starve the others.
	for (uint8_t i = 0; i < w5500_sockets; ++i) {
		const uint8_t s = static_cast<uint8_t>((_rr + i) % w5500_sockets);
		Sock &k = _sock[s];
		if (k.rxPending && k.mode == Sock::Established && _host && _host->rxSpace(s) > 0) {
			_rr = static_cast<uint8_t>(s + 1);
			return rxBegin(s, nowMs);
		}
	}

	// 5. Data to send.
	for (uint8_t i = 0; i < w5500_sockets; ++i) {
		const uint8_t s = static_cast<uint8_t>((_rr + i) % w5500_sockets);
		Sock &k = _sock[s];
		if (k.mode != Sock::Established || k.sendInFlight) continue;
		if (k.txBlocked) {
			if (!after(nowMs, k.txBlockedAt, _param.busyPollMs)) continue;
			k.txBlocked = false;
		}
		if (_host && _host->txPending(s) > 0) {
			_rr = static_cast<uint8_t>(s + 1);
			return txBegin(s, nowMs);
		}
	}

	// 6. The peer has closed, everything it sent has been delivered,
	//    and everything we had to send has gone: close our side.
	for (uint8_t s = 0; s < w5500_sockets; ++s) {
		Sock &k = _sock[s];
		if (k.mode == Sock::Established && k.peerClosed && !k.rxPending && !k.sendInFlight
		    && !(_host && _host->txPending(s) > 0)) {
			k.mode = Sock::Closing;
			k.since = k.lastSvc = nowMs;
			return command(s, w5500_CR_DISCON, St::idle, nowMs);
		}
	}

	// 7. Look for news nobody told us about.
	if (_sirRecheck || after(nowMs, _lastSir, _param.pollMs)) {
		_sirRecheck = false;
		return readSir(nowMs);
	}

	// 8. Link.
	if (after(nowMs, _lastPhy, _param.linkPollMs)) {
		enter(St::read_phy, nowMs);
		return 0;
	}

	return idleWait(nowMs);
}

template <typename TTransport>
uint32_t w5500<TTransport>::idleWait(uint32_t nowMs) const {
	uint32_t wait = remaining(nowMs, _lastSir, _param.pollMs);
	const uint32_t phy = remaining(nowMs, _lastPhy, _param.linkPollMs);
	if (phy < wait) wait = phy;
	for (uint8_t s = 0; s < w5500_sockets; ++s) {
		const Sock &k = _sock[s];
		if (k.mode == Sock::Connecting || k.mode == Sock::Closing || k.sendInFlight) {
			const uint32_t w = remaining(nowMs, k.lastSvc, _param.busyPollMs);
			if (w < wait) wait = w;
		}
		if (k.txBlocked) {
			const uint32_t w = remaining(nowMs, k.txBlockedAt, _param.busyPollMs);
			if (w < wait) wait = w;
		}
	}
	return wait == 0 ? 1 : wait;
}

template <typename TTransport>
uint32_t w5500<TTransport>::startRequest(uint8_t s, uint32_t nowMs) {
	Sock &k = _sock[s];
	const typename Sock::Req req = k.req;
	k.req = Sock::NoReq;

	if (req == Sock::ReqClose) {
		switch (k.mode) {
		case Sock::Closed:
			emit(s, SocketEvent::Closed);
			return 0;
		case Sock::Closing:
			return 0;	// already on its way; Closed follows
		case Sock::Established:
			// Graceful: FIN, then wait for SOCK_CLOSED (or the timeout).
			// Anything still unread is thrown away.
			k.mode = Sock::Closing;
			k.rxPending = false;
			k.since = k.lastSvc = nowMs;
			return command(s, w5500_CR_DISCON, St::idle, nowMs);
		default:
			k.mode = Sock::Closed;
			emit(s, SocketEvent::Closed);
			return command(s, w5500_CR_CLOSE, St::idle, nowMs);
		}
	}

	// Connect or listen. Whatever the socket was doing is dropped
	// quietly: the host has already moved on from it.
	if (_param.rxBufKb[s] == 0 || _param.txBufKb[s] == 0) {
		k.mode = Sock::Closed;
		emit(s, SocketEvent::Failed);
		return 0;
	}
	const IpAddress ip = k.reqIp;
	const uint16_t port = k.reqPort;
	uint16_t local = k.reqLocalPort;
	if (local == 0) {
		local = _nextPort;
		_nextPort = static_cast<uint16_t>(_nextPort == 65535 ? 49152 : _nextPort + 1);
	}
	k = Sock();
	k.mode = Sock::Opening;
	k.opening_listen = (req == Sock::ReqListen);
	k.reqIp = ip;
	k.reqPort = port;
	k.reqLocalPort = local;
	_cur = s;
	return command(s, w5500_CR_CLOSE, St::open_mode, nowMs);
}

// Acts on one socket's Sn_IR (already cleared on the chip) and Sn_SR.
template <typename TTransport>
uint32_t w5500<TTransport>::evaluate(uint8_t s, uint8_t ir, uint8_t sr, uint32_t nowMs) {
	Sock &k = _sock[s];
	if (ir & w5500_IR_SENDOK) k.sendInFlight = false;
	if (ir & w5500_IR_RECV) k.rxPending = true;

	switch (k.mode) {
	case Sock::Connecting:
	case Sock::Listening:
		if (sr == w5500_SOCK_ESTABLISHED || sr == w5500_SOCK_CLOSE_WAIT) {
			k.mode = Sock::Established;
			k.rxPending = true;	// the peer may have sent already
			k.peerClosed = (sr == w5500_SOCK_CLOSE_WAIT);
			emit(s, SocketEvent::Connected);
		} else if ((ir & w5500_IR_TIMEOUT) || sr == w5500_SOCK_CLOSED) {
			k.mode = Sock::Closed;
			emit(s, SocketEvent::Failed);
			return command(s, w5500_CR_CLOSE, St::svc_next, nowMs);
		}
		break;
	case Sock::Established:
		if ((ir & w5500_IR_TIMEOUT) || sr == w5500_SOCK_CLOSED) {
			// Retransmissions ran out, or the peer reset the connection.
			const bool timedOut = (ir & w5500_IR_TIMEOUT) != 0;
			k = Sock();
			emit(s, timedOut ? SocketEvent::Failed : SocketEvent::Closed);
			return command(s, w5500_CR_CLOSE, St::svc_next, nowMs);
		}
		if ((ir & w5500_IR_DISCON) || sr == w5500_SOCK_CLOSE_WAIT) {
			k.peerClosed = true;
			k.rxPending = true;	// drain before closing our side
		}
		break;
	case Sock::Closing:
		if (sr == w5500_SOCK_CLOSED || (ir & w5500_IR_TIMEOUT)) {
			k = Sock();
			emit(s, SocketEvent::Closed);
		}
		break;
	default:
		break;
	}
	enter(St::svc_next, nowMs);
	return 0;
}

// The chip is unusable: every socket is lost, and the error state
// starts again from reset after a pause.
template <typename TTransport>
uint32_t w5500<TTransport>::fail(uint32_t nowMs) {
	dropAll(SocketEvent::Failed);
	_ready = false;
	_irq = false;
	_sirRecheck = false;
	if (_link) {
		_link = false;
		emitDev(DeviceEvent::LinkDown);
	}
	emitDev(DeviceEvent::Failed);
	enter(St::error, nowMs);
	return 0;
}

template <typename TTransport>
void w5500<TTransport>::dropAll(SocketEvent ev) {
	for (uint8_t s = 0; s < w5500_sockets; ++s) {
		Sock &k = _sock[s];
		const bool open = k.mode != Sock::Closed;
		const bool opening = k.req == Sock::ReqConnect || k.req == Sock::ReqListen;
		const bool closing = k.req == Sock::ReqClose;
		k = Sock();
		if (open || opening) emit(s, ev);
		else if (closing) emit(s, SocketEvent::Closed);
	}
}

template <typename TTransport>
void w5500<TTransport>::applyPhy() {
	const bool up = (_phy & w5500_PHY_LNK) != 0;
	if (up == _link) return;
	_link = up;
	emitDev(up ? DeviceEvent::LinkUp : DeviceEvent::LinkDown);
}

template <typename TTransport>
bool w5500<TTransport>::bufConfigValid() const {
	unsigned rx = 0, tx = 0;
	for (uint8_t s = 0; s < w5500_sockets; ++s) {
		const uint8_t r = _param.rxBufKb[s], t = _param.txBufKb[s];
		const bool rOk = r == 0 || r == 1 || r == 2 || r == 4 || r == 8 || r == 16;
		const bool tOk = t == 0 || t == 1 || t == 2 || t == 4 || t == 8 || t == 16;
		if (!rOk || !tOk) return false;
		rx += r;
		tx += t;
	}
	return rx <= 16 && tx <= 16;
}

} /* namespace W5500 */

#endif /* W5500_TPP_ */
