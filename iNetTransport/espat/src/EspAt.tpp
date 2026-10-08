/*
 * EspAt.tpp
 *
 *  espat's member definitions. EspAt.h includes this file at its end,
 *  since templates have to be visible wherever they are used: include
 *  EspAt.h, not this file.
 */

#ifndef ESPAT_TPP_
#define ESPAT_TPP_

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../inc/EspAt.h"

namespace ESPAT {

typedef espat_state_t St;

namespace detail {

// The first a.b.c.d in s, quoted or not.
inline bool parseIp(const char *s, IpAddress &out) {
	for (; *s; ++s) {
		if (*s < '0' || *s > '9') continue;
		const char *p = s;
		unsigned v[4];
		int i = 0;
		for (; i < 4; ++i) {
			if (*p < '0' || *p > '9') break;
			unsigned n = 0;
			int digits = 0;
			while (*p >= '0' && *p <= '9' && digits < 4) { n = n * 10 + static_cast<unsigned>(*p - '0'); ++p; ++digits; }
			if (n > 255) break;
			v[i] = n;
			if (i < 3) {
				if (*p != '.') break;
				++p;
			}
		}
		if (i == 4) {
			out = IpAddress(static_cast<uint8_t>(v[0]), static_cast<uint8_t>(v[1]),
			                static_cast<uint8_t>(v[2]), static_cast<uint8_t>(v[3]));
			return true;
		}
		while (*s >= '0' && *s <= '9') ++s;	// not an address: skip this number
		if (!*s) break;
	}
	return false;
}

inline bool startsWith(const char *s, const char *prefix) { return strncmp(s, prefix, strlen(prefix)) == 0; }

// "<digit>,<word>" -> the digit, if the rest is exactly word.
inline int linkEvent(const char *s, const char *word) {
	if (s[0] < '0' || s[0] > '4' || s[1] != ',') return -1;
	return strcmp(s + 2, word) == 0 ? s[0] - '0' : -1;
}

// "Www Mmm dd hh:mm:ss yyyy" (asctime, UTC) as Unix seconds; 0 if it
// doesn't parse.
inline uint64_t parseAsctime(const char *s) {
	static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
	char tok[5][12];
	int n = 0;
	while (*s && n < 5) {
		while (*s == ' ') ++s;
		int i = 0;
		while (*s && *s != ' ' && i < 11) tok[n][i++] = *s++;
		tok[n][i] = 0;
		if (i) ++n;
		while (*s && *s != ' ') ++s;
	}
	if (n < 5 || strlen(tok[1]) != 3) return 0;
	const char *m = strstr(months, tok[1]);
	if (!m || (m - months) % 3) return 0;
	const int mon = static_cast<int>(m - months) / 3 + 1;
	const int day = atoi(tok[2]);
	int hh = 0, mm = 0, ss = 0;
	if (sscanf(tok[3], "%d:%d:%d", &hh, &mm, &ss) != 3) return 0;
	int y = atoi(tok[4]);
	if (y < 1970 || day < 1 || day > 31 || hh > 23 || mm > 59 || ss > 60) return 0;
	// Days from civil (H. Hinnant), for a proleptic Gregorian date.
	y -= mon <= 2;
	const int era = y / 400;
	const int yoe = y - era * 400;
	const int doy = (153 * (mon + (mon > 2 ? -3 : 9)) + 2) / 5 + day - 1;
	const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	const int64_t days = static_cast<int64_t>(era) * 146097 + doe - 719468;
	return static_cast<uint64_t>(days) * 86400u + static_cast<uint64_t>(hh * 3600 + mm * 60 + ss);
}

// Skips a quoted field (backslash escapes), returns what follows it.
inline const char *skipQuoted(const char *s) {
	if (*s != '"') return s;
	for (++s; *s && *s != '"'; ++s) {
		if (*s == '\\' && s[1]) ++s;
	}
	return *s ? s + 1 : s;
}

} // namespace detail

template <typename TTransport>
template <typename... TArgs>
espat<TTransport>::espat(const espat_param_t &param, TArgs&&... transportArgs)
	: TTransport(std::forward<TArgs>(transportArgs)...), _param(param) {
	this->setRxSink(*this);	// the transport is fully built; this starts reception too
}

// ---- requests, from the driver thread ----

template <typename TTransport>
void espat<TTransport>::configure(const NetConfig &cfg) {
	_cfg = cfg;
	_configured = true;
	_reconfigure = true;
}

template <typename TTransport>
bool espat<TTransport>::connect(uint8_t s, const IpAddress &ip, uint16_t port, uint16_t localPort) {
	(void)localPort;	// the module picks its own
	if (s >= espat_links || !_configured) return false;
	_sock[s].req = Sock::ReqConnect;
	_sock[s].ip = ip;
	_sock[s].port = port;
	return true;
}

template <typename TTransport>
bool espat<TTransport>::listen(uint8_t s, uint16_t port) {
	if (s >= espat_links || !_configured) return false;
	_sock[s].req = Sock::ReqListen;
	_sock[s].port = port;
	return true;
}

template <typename TTransport>
void espat<TTransport>::close(uint8_t s) {
	if (s >= espat_links) return;
	if (!_ready) {
		_sock[s].req = Sock::NoReq;
		emit(s, SocketEvent::Closed);
		return;
	}
	_sock[s].req = Sock::ReqClose;
}

template <typename TTransport>
bool espat<TTransport>::resolve(const char *name) {
	// ESP-AT takes domain names of up to 64 characters.
	if (!_configured || name == nullptr || strlen(name) > 64) return false;
	strcpy(_dnsName, name);
	_dnsReq = true;
	++_dnsGen;	// an answer still coming for an earlier name won't count
	return true;
}

template <typename TTransport>
bool espat<TTransport>::requestTime(const char *server) {
	if (!_configured || server == nullptr || strlen(server) > 64) return false;
	strcpy(_ntpServer, server);
	_ntpReq = true;
	_ntpPolling = false;
	return true;
}

template <typename TTransport>
void espat<TTransport>::join(const char *ssid, const char *passphrase) {
	strncpy(_ssid, ssid ? ssid : "", sizeof _ssid - 1);
	strncpy(_pass, passphrase ? passphrase : "", sizeof _pass - 1);
	_joinReq = true;
	_leaveReq = false;
}

template <typename TTransport>
void espat<TTransport>::leave() {
	_leaveReq = true;
	_joinReq = false;
	_ssid[0] = 0;	// and don't rejoin after a module reset
}

// ---- receive ----

// ISR. Wakes the driver thread when a line ends, at the ">" prompt,
// or when the ring is half full — not for every byte.
template <typename TTransport>
void espat<TTransport>::onByteReceived(uint8_t c) {
	const uint16_t mask = espat_ring_bytes - 1;
	const uint16_t h = _head.load(std::memory_order_relaxed);
	const uint16_t next = static_cast<uint16_t>((h + 1) & mask);
	const uint16_t t = _tail.load(std::memory_order_acquire);
	if (next == t) {
		_overflow.store(true, std::memory_order_relaxed);
		if (_host) _host->wakeFromIsr();
		return;
	}
	_ring[h] = c;
	_head.store(next, std::memory_order_release);
	const uint16_t used = static_cast<uint16_t>((next - t) & mask);
	if ((c == '\n' || c == '>' || used >= espat_ring_bytes / 2) && _host) _host->wakeFromIsr();
}

template <typename TTransport>
void espat<TTransport>::drain(uint32_t nowMs) {
	(void)nowMs;
	const uint16_t mask = espat_ring_bytes - 1;
	uint16_t t = _tail.load(std::memory_order_relaxed);
	const uint16_t h = _head.load(std::memory_order_acquire);
	while (t != h) {
		feed(_ring[t]);
		t = static_cast<uint16_t>((t + 1) & mask);
	}
	_tail.store(t, std::memory_order_release);
}

template <typename TTransport>
void espat<TTransport>::feed(uint8_t c) {
	if (_binLeft != 0) {	// inside a +CIPRECVDATA payload: raw bytes, newlines and all
		if (_binGot < sizeof _chunk) _chunk[_binGot++] = c;
		if (--_binLeft == 0) binaryDone();
		return;
	}
	if (c == '\n') {
		while (_lineLen > 0 && _line[_lineLen - 1] == '\r') --_lineLen;
		_line[_lineLen] = 0;
		_lineLen = 0;
		onLine(_line);
		return;
	}
	if (c == '>' && _lineLen == 0 && _state == St::wait && _cmd == Cmd::Send && _sendStage == 0) {
		_res = Res::Prompt;
		return;
	}
	// "+CIPRECVDATA:<len>," (v2.x) or "+CIPRECVDATA,<len>:" (ESP8266 1.7):
	// the payload starts straight after the second separator.
	if ((c == ',' || c == ':') && _lineLen > 13 && strncmp(_line, "+CIPRECVDATA", 12) == 0) {
		const char first = _line[12];
		if ((first == ':' && c == ',') || (first == ',' && c == ':')) {
			unsigned n = 0;
			bool digits = true;
			for (uint8_t i = 13; i < _lineLen; ++i) {
				if (_line[i] < '0' || _line[i] > '9') { digits = false; break; }
				n = n * 10 + static_cast<unsigned>(_line[i] - '0');
			}
			if (digits && n <= 0xFFFF) {
				_lineLen = 0;
				_binGot = 0;
				_binLeft = static_cast<uint16_t>(n);
				if (n == 0) binaryDone();
				return;
			}
		}
	}
	if (_lineLen < espat_line_max - 1) _line[_lineLen++] = static_cast<char>(c);
}

template <typename TTransport>
void espat<TTransport>::binaryDone() {
	if (_state != St::wait || _cmd != Cmd::RecvData || _cmdSock < 0) return;
	uint16_t n = _binGot;
	if (n > _xLen) n = _xLen;	// never more than the host made room for
	if (n != 0 && _host) _host->rxDeliver(static_cast<uint8_t>(_cmdSock), _chunk, n);
	_stRx.fetch_add(n, std::memory_order_relaxed);
}

template <typename TTransport>
void espat<TTransport>::onLine(const char *l) {
	using namespace detail;
	if (l[0] == 0) return;

	Res r = Res::Pending;
	if (strcmp(l, "OK") == 0) r = Res::Ok;
	else if (strcmp(l, "ERROR") == 0 || strcmp(l, "FAIL") == 0 || strcmp(l, "SEND FAIL") == 0) r = Res::Error;
	else if (strcmp(l, "SEND OK") == 0) r = Res::SendOk;
	else if (startsWith(l, "busy ")) r = Res::Busy;
	if (r != Res::Pending) {
		if (_state == St::wait && _res == Res::Pending) _res = r;
		return;
	}

	int link;
	if (strcmp(l, "ready") == 0) {
		// Expected after a reset; anywhere else the module rebooted by itself.
		const bool expected = _state == St::wait_ready || _state == St::reset_hold
		                      || (_state == St::wait && (_cmd == Cmd::Rst || _cmd == Cmd::Probe)) || _state == St::probe;
		if (expected) _resetDone = true;
		else if (_ready) _rebooted = true;
		if (_state == St::wait_ready) _since -= espat_ready_timeout_ms;	// done waiting
	} else if (strcmp(l, "WIFI DISCONNECT") == 0) {
		linkLost();
	} else if (strcmp(l, "WIFI GOT IP") == 0) {
		_needAddr = true;
	} else if ((link = linkEvent(l, "CONNECT")) >= 0) {
		if (!(_state == St::wait && _cmd == Cmd::Start && _cmdLink == link)) incoming(static_cast<uint8_t>(link));
	} else if ((link = linkEvent(l, "CLOSED")) >= 0) {
		linkClosed(static_cast<uint8_t>(link));
	} else if (startsWith(l, "+IPD,")) {
		const int lk = l[5] - '0';
		if (lk >= 0 && lk < espat_links && _linkOwner[lk] >= 0) {
			_sock[_linkOwner[lk]].rxPending = true;
			if (_state == St::wait && _cmd == Cmd::RecvData && _cmdLink == lk) _ipdDuring = true;
		}
	} else if (startsWith(l, "+CIPSTA:ip:")) {
		parseIp(l + 11, _q.ip);
	} else if (startsWith(l, "+CIPSTA:gateway:")) {
		parseIp(l + 16, _q.gateway);
	} else if (startsWith(l, "+CIPSTA:netmask:")) {
		parseIp(l + 16, _q.subnet);
	} else if (startsWith(l, "+CIPDNS")) {
		const char *p = strchr(l, ':');
		if (p) parseIp(p + 1, _q.dns);
	} else if (startsWith(l, "+CWJAP:\"")) {
		// "ssid","bssid",channel,rssi,...
		const char *p = skipQuoted(l + 7);
		if (*p == ',') p = skipQuoted(p + 1);
		if (*p == ',') p = strchr(p + 1, ',');
		if (p && *p == ',') _rssi = static_cast<int8_t>(atoi(p + 1));
	} else if (startsWith(l, "+CIPDOMAIN:")) {
		parseIp(l + 11, _dnsResult);
	} else if (startsWith(l, "+CIPSNTPTIME:")) {
		_ntpSec = parseAsctime(l + 13);
	} else if (startsWith(l, "AT version:")) {
		strncpy(_firmware, l, sizeof _firmware - 1);
		_firmware[sizeof _firmware - 1] = 0;
	} else if (strcmp(l, "No AP") == 0) {
		if (_state == St::wait && _cmd == Cmd::QueryAp) linkLost();
	}
	// Everything else ("WIFI CONNECTED", "Recv N bytes", echoes, boot
	// messages) is ignored.
}

// ---- sockets and links ----

template <typename TTransport>
void espat<TTransport>::closeSock(uint8_t s, SocketEvent ev) {
	Sock &k = _sock[s];
	if (k.link >= 0 && _linkOwner[k.link] == static_cast<int8_t>(s)) _linkOwner[k.link] = -1;
	const typename Sock::Req req = k.req;
	k = Sock();
	k.req = req;	// a request already made stands
	emit(s, ev);
}

// A peer connected to our server, on link l.
template <typename TTransport>
void espat<TTransport>::incoming(uint8_t l) {
	if (l >= espat_links || _linkOwner[l] >= 0) return;
	for (uint8_t s = 0; s < espat_links; ++s) {
		Sock &k = _sock[s];
		if (k.mode == Sock::Listening && k.req == Sock::NoReq) {
			k.mode = Sock::Established;
			k.link = static_cast<int8_t>(l);
			k.rxPending = false;
			k.peerClosed = false;
			_linkOwner[l] = static_cast<int8_t>(s);
			emit(s, SocketEvent::Connected);
			return;
		}
	}
	_stray |= static_cast<uint8_t>(1u << l);	// nobody listening: close it
}

template <typename TTransport>
void espat<TTransport>::linkClosed(uint8_t l) {
	if (l >= espat_links) return;
	_stray &= static_cast<uint8_t>(~(1u << l));
	const int8_t s = _linkOwner[l];
	if (s < 0) return;
	// Our own CIPCLOSE or a failing CIPSTART: its result settles it.
	if (_state == St::wait && (_cmd == Cmd::Close || _cmd == Cmd::Start) && _cmdLink == static_cast<int8_t>(l)) return;
	Sock &k = _sock[s];
	if (k.mode == Sock::Established) k.peerClosed = true;	// deliver what the module still holds, then Closed
}

template <typename TTransport>
int8_t espat<TTransport>::freeLink() const {
	for (uint8_t l = 0; l < espat_links; ++l) {
		if (_linkOwner[l] < 0 && !(_stray & (1u << l))) return static_cast<int8_t>(l);
	}
	return -1;
}

// Not joined any more: the module has closed every connection.
template <typename TTransport>
void espat<TTransport>::linkLost() {
	for (uint8_t s = 0; s < espat_links; ++s) {
		const typename Sock::Mode m = _sock[s].mode;
		if (m == Sock::Established || m == Sock::Connecting || m == Sock::Closing) closeSock(s, SocketEvent::Failed);
	}
	_stray = 0;
	_needAddr = false;
	if (_link) {
		_link = false;
		emitDev(DeviceEvent::LinkDown);
	}
	if (!_net.ip.isZero()) {
		_net = NetConfig();
		if (_host) _host->addressChanged(_net);
	}
}

// ---- commands ----

template <typename TTransport>
void espat<TTransport>::formatCmd(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	const int n = vsnprintf(_cmdBuf, sizeof _cmdBuf - 2, fmt, ap);
	va_end(ap);
	_cmdLen = n < 0 ? 0 : (static_cast<size_t>(n) > sizeof _cmdBuf - 3 ? sizeof _cmdBuf - 3 : static_cast<size_t>(n));
}

// ESP-AT wants , " and \ inside a quoted string escaped with \.
template <typename TTransport>
size_t espat<TTransport>::appendEscaped(size_t at, const char *s) {
	for (; *s && at < sizeof _cmdBuf - 4; ++s) {
		if (*s == ',' || *s == '"' || *s == '\\') _cmdBuf[at++] = '\\';
		_cmdBuf[at++] = *s;
	}
	return at;
}

// Sends _cmdBuf[0.._cmdLen) plus CRLF and waits for the result. The
// state is set before the write: a module (or a test) can answer
// before write() returns.
template <typename TTransport>
bool espat<TTransport>::issue(Cmd cmd, uint32_t timeoutMs, uint32_t nowMs) {
	_cmdBuf[_cmdLen++] = '\r';
	_cmdBuf[_cmdLen++] = '\n';
	_cmd = cmd;
	_res = Res::Pending;
	_cmdTimeout = timeoutMs;
	_busyTries = 0;
	_ipdDuring = false;
	_stCommands.fetch_add(1, std::memory_order_relaxed);
	if (cmd == Cmd::Rst) _resetDone = false;
	enter(St::wait, nowMs);
	if (!this->write(reinterpret_cast<const uint8_t *>(_cmdBuf), _cmdLen)) {
		// UART still busy with the last transmission. Unusual (every
		// command waits for an answer); the timeout covers it.
		_res = Res::Busy;
	}
	return true;
}

template <typename TTransport>
bool espat<TTransport>::nextInit(uint32_t nowMs) {
	switch (_initIdx) {
	case 0: formatCmd("ATE0"); break;					// no echo
	case 1: _firmware[0] = 0; formatCmd("AT+GMR"); break;	// firmware version, for the log
	case 2: formatCmd("AT+CWMODE=1"); break;			// station
	case 3: formatCmd("AT+CIPMUX=1"); break;			// links 0..4
	case 4: formatCmd("AT+CIPRECVMODE=1"); break;		// passive receive
	case 5: formatCmd("AT+CWAUTOCONN=0"); break;		// join only when asked
	case 6:
		if (_cfg.dhcp) {
			formatCmd("AT+CWDHCP=1,1");					// station DHCP on (v2.x and 1.7 alike)
		} else {
			const IpAddress &a = _cfg.ip, &g = _cfg.gateway, &m = _cfg.subnet;
			formatCmd("AT+CIPSTA=\"%u.%u.%u.%u\",\"%u.%u.%u.%u\",\"%u.%u.%u.%u\"",
			          a.b[0], a.b[1], a.b[2], a.b[3], g.b[0], g.b[1], g.b[2], g.b[3], m.b[0], m.b[1], m.b[2], m.b[3]);
		}
		break;
	default:
		_ready = true;
		_lastAp = nowMs;
		emitDev(DeviceEvent::Ready);
		if (_ssid[0]) _joinReq = true;	// a module reset lost the network: join it again
		enter(St::idle, nowMs);
		return false;
	}
	return issue(Cmd::Init, espat_cmd_timeout_ms, nowMs);
}

template <typename TTransport>
void espat<TTransport>::finish(Res res, uint32_t nowMs) {
	enter(St::idle, nowMs);
	if (res == Res::Busy && _cmd != Cmd::Probe) {
		if (++_busyTries > espat_busy_tries) { fail(nowMs); return; }
		const uint8_t tries = _busyTries;
		enter(St::busy_wait, nowMs);
		_busyTries = tries;
		return;
	}
	const bool ok = res == Res::Ok;

	switch (_cmd) {
	case Cmd::Probe:
		if (ok) {
			if (_resetDone) {
				_initIdx = 0;
				enter(St::init, nowMs);
			} else {
				formatCmd("AT+RST");
				issue(Cmd::Rst, espat_cmd_timeout_ms, nowMs);
			}
		} else if (++_tries >= espat_probe_tries) {
			fail(nowMs);
		} else {
			enter(St::probe, nowMs);
		}
		break;
	case Cmd::Rst:
		if (!ok) { fail(nowMs); break; }
		enter(St::wait_ready, nowMs);
		if (_resetDone) _since -= espat_ready_timeout_ms;	// "ready" already seen
		break;
	case Cmd::Init:
		if (!ok) { fail(nowMs); break; }
		++_initIdx;
		enter(St::init, nowMs);
		break;
	case Cmd::Join:
		if (ok) _needAddr = true;	// "WIFI GOT IP" says the same, usually first
		else if (res == Res::Timeout) fail(nowMs);
		else emitDev(DeviceEvent::JoinFailed);
		break;
	case Cmd::Leave:
		if (res == Res::Timeout) { fail(nowMs); break; }
		linkLost();
		break;
	case Cmd::QueryIp:
		if (res == Res::Timeout) { fail(nowMs); break; }
		formatCmd("AT+CIPDNS?");
		issue(Cmd::QueryDns, espat_cmd_timeout_ms, nowMs);
		break;
	case Cmd::QueryDns: {
		if (res == Res::Timeout) { fail(nowMs); break; }
		if (_q.ip.isZero()) break;	// no address after all; the next "WIFI GOT IP" asks again
		_q.mac = _cfg.mac;
		_q.dhcp = _cfg.dhcp;
		const bool changed = _q.ip != _net.ip || _q.subnet != _net.subnet || _q.gateway != _net.gateway || _q.dns != _net.dns;
		if (changed) {
			if (!_net.ip.isZero() && _q.ip != _net.ip) {
				for (uint8_t s = 0; s < espat_links; ++s) {
					if (_sock[s].mode == Sock::Established) closeSock(s, SocketEvent::Failed);
				}
			}
			_net = _q;
			if (_host) _host->addressChanged(_net);
		}
		if (!_link) {
			_link = true;
			_lastAp = nowMs;
			emitDev(DeviceEvent::LinkUp);
		}
		break;
	}
	case Cmd::QueryAp:
		if (res == Res::Timeout) fail(nowMs);	// "No AP" was handled as it arrived
		break;
	case Cmd::Start: {
		Sock &k = _sock[_cmdSock];
		if (ok) {
			k.mode = Sock::Established;
			emit(static_cast<uint8_t>(_cmdSock), SocketEvent::Connected);
		} else if (res == Res::Timeout) {
			fail(nowMs);
		} else {
			closeSock(static_cast<uint8_t>(_cmdSock), SocketEvent::Failed);
		}
		break;
	}
	case Cmd::Server:
		if (ok) {
			_serverOn = true;
			_serverPort = _sock[_cmdSock].port;
			_needSto = true;
			emit(static_cast<uint8_t>(_cmdSock), SocketEvent::Listening);
		} else if (res == Res::Timeout) {
			fail(nowMs);
		} else {
			closeSock(static_cast<uint8_t>(_cmdSock), SocketEvent::Failed);
		}
		break;
	case Cmd::ServerTimeout:
		if (res == Res::Timeout) fail(nowMs);
		break;
	case Cmd::Domain:
		if (res == Res::Timeout) { fail(nowMs); break; }
		_dnsBusy = false;
		// Only the answer to the latest resolve() counts; a newer one
		// is already waiting to be sent.
		if (_dnsCmdGen == _dnsGen && _host) _host->resolved(ok && !_dnsResult.isZero(), _dnsResult);
		break;
	case Cmd::SntpCfg:
		if (res == Res::Timeout) { fail(nowMs); break; }
		if (!ok) {
			_ntpBusy = false;
			_ntpConfigured[0] = 0;
			if (_host) _host->timeReceived(false, 0, nowMs);
			break;
		}
		strcpy(_ntpConfigured, _ntpServer);
		_ntpPolling = true;
		_ntpPollStart = nowMs;
		_ntpLastPoll = nowMs - espat_sntp_poll_ms;	// ask straight away
		break;
	case Cmd::SntpTime:
		if (res == Res::Timeout) { fail(nowMs); break; }
		// Until the module has synchronised it reports 1970.
		if (ok && _ntpSec >= 1577836800u /* 2020 */) {
			_ntpPolling = false;
			_ntpBusy = false;
			// It reports whole seconds: the middle of the second is the best guess.
			if (_host) _host->timeReceived(true, _ntpSec * 1000u + 500u, nowMs);
		}
		break;
	case Cmd::Close:
		if (res == Res::Timeout) { fail(nowMs); break; }
		// ERROR too: the link was already gone.
		if (_cmdSock >= 0) closeSock(static_cast<uint8_t>(_cmdSock), SocketEvent::Closed);
		break;
	case Cmd::RecvData: {
		if (res == Res::Timeout) { fail(nowMs); break; }
		Sock &k = _sock[_cmdSock];
		// Had less than we asked for: that was all of it — unless more
		// was announced meanwhile.
		if (!ok || _binGot < _xLen) k.rxPending = _ipdDuring;
		break;
	}
	case Cmd::Send: {
		if (res == Res::Timeout) { fail(nowMs); break; }
		const uint8_t s = static_cast<uint8_t>(_cmdSock);
		if (_sendStage == 0 && res == Res::Prompt) {
			_sendStage = 1;
			_res = Res::Pending;
			enter(St::wait, nowMs);
			_since = nowMs;
			if (!this->write(_chunk, _xLen)) _res = Res::Error;
			break;
		}
		if (res == Res::Ok) {	// "OK" to the command line itself, before ">"
			_res = Res::Pending;
			enter(St::wait, nowMs);
			break;
		}
		if (_sendStage == 1 && res == Res::SendOk) {	// sent
			_stTx.fetch_add(_xLen, std::memory_order_relaxed);
			break;
		}
		// ERROR or SEND FAIL: the connection is no good.
		const int8_t link = _sock[s].link;
		closeSock(s, SocketEvent::Failed);
		if (link >= 0) _stray |= static_cast<uint8_t>(1u << link);
		break;
	}
	default:
		break;
	}
}

// ---- the state machine ----

template <typename TTransport>
uint32_t espat<TTransport>::poll(uint32_t nowMs) {
	for (int i = 0; i < 32; ++i) {
		drain(nowMs);
		const uint32_t wait = step(nowMs);
		if (wait != 0) return wait;
	}
	return 0;
}

template <typename TTransport>
uint32_t espat<TTransport>::step(uint32_t nowMs) {
	if (_overflow.exchange(false)) {
		// Bytes were lost: whatever was being parsed can't be trusted.
		_stOverflows.fetch_add(1, std::memory_order_relaxed);
		return fail(nowMs);
	}
	if (_rebooted) {
		_rebooted = false;
		return fail(nowMs);
	}

	switch (_state) {
	case St::unconfigured:
		if (!_reconfigure) return 1000;
		_reconfigure = false;
		_resetDone = false;
		_tries = 0;
		if (_param.hardReset) {
			_param.hardReset(true);
			enter(St::reset_hold, nowMs);
		} else {
			enter(St::probe, nowMs);
		}
		return 0;
	case St::reset_hold:
		if (!elapsed(nowMs, 20)) return remaining(nowMs, _since, 20);
		_resetDone = false;
		_param.hardReset(false);
		enter(St::wait_ready, nowMs);
		return 0;
	case St::wait_ready:
		// "ready" (seen in onLine) or the timeout — some firmware prints
		// it at another baud rate — and then make sure it answers.
		if (!elapsed(nowMs, espat_ready_timeout_ms)) return remaining(nowMs, _since, espat_ready_timeout_ms);
		_resetDone = true;
		_tries = 0;
		enter(St::probe, nowMs);
		return 0;
	case St::probe:
		formatCmd("AT");
		issue(Cmd::Probe, espat_probe_interval_ms, nowMs);
		return 0;
	case St::init:
		nextInit(nowMs);
		return 0;
	case St::idle:
		return schedule(nowMs);
	case St::wait:
		if (_res != Res::Pending) {
			const Res r = _res;
			_res = Res::Pending;
			finish(r, nowMs);
			return 0;
		}
		if (elapsed(nowMs, _cmdTimeout)) {
			if (_cmd != Cmd::Probe) _stTimeouts.fetch_add(1, std::memory_order_relaxed);
			finish(Res::Timeout, nowMs);
			return 0;
		}
		return remaining(nowMs, _since, _cmdTimeout);
	case St::busy_wait: {
		if (!elapsed(nowMs, espat_busy_retry_ms)) return remaining(nowMs, _since, espat_busy_retry_ms);
		const uint8_t tries = _busyTries;
		_cmdLen -= 2;	// issue() adds the CRLF again
		issue(_cmd, _cmdTimeout, nowMs);
		_busyTries = tries;
		if (_cmd == Cmd::Send) _sendStage = 0;
		return 0;
	}
	case St::error:
		if (!elapsed(nowMs, espat_error_backoff_ms)) return remaining(nowMs, _since, espat_error_backoff_ms);
		_reconfigure = _configured;
		enter(St::unconfigured, nowMs);
		return 0;
	}
	return fail(nowMs);
}

// What to do next, most urgent first. 0 having started something, else
// how long nothing needs doing (the UART ISR wakes us before that if
// the module says anything).
template <typename TTransport>
uint32_t espat<TTransport>::schedule(uint32_t nowMs) {
	if (_reconfigure) {
		fail(nowMs);
		_since -= espat_error_backoff_ms;	// no back-off for a reconfigure
		return 0;
	}

	// Links nobody wants.
	if (_stray) {
		for (uint8_t l = 0; l < espat_links; ++l) {
			if (!(_stray & (1u << l))) continue;
			_stray = static_cast<uint8_t>(_stray & ~(1u << l));
			_cmdSock = -1;
			_cmdLink = static_cast<int8_t>(l);
			formatCmd("AT+CIPCLOSE=%u", l);
			issue(Cmd::Close, espat_cmd_timeout_ms, nowMs);
			return 0;
		}
	}

	// Closes.
	for (uint8_t s = 0; s < espat_links; ++s) {
		Sock &k = _sock[s];
		if (k.req != Sock::ReqClose) continue;
		k.req = Sock::NoReq;
		if (k.link < 0 || k.mode == Sock::Closed) {	// listening, or nothing at all
			closeSock(s, SocketEvent::Closed);
			return 0;
		}
		k.mode = Sock::Closing;
		_cmdSock = static_cast<int8_t>(s);
		_cmdLink = k.link;
		formatCmd("AT+CIPCLOSE=%d", k.link);
		issue(Cmd::Close, espat_cmd_timeout_ms, nowMs);
		return 0;
	}

	// The network.
	if (_leaveReq) {
		_leaveReq = false;
		formatCmd("AT+CWQAP");
		issue(Cmd::Leave, espat_cmd_timeout_ms, nowMs);
		return 0;
	}
	if (_joinReq) {
		_joinReq = false;
		size_t at = 0;
		memcpy(_cmdBuf, "AT+CWJAP=\"", 10);
		at = appendEscaped(10, _ssid);
		memcpy(_cmdBuf + at, "\",\"", 3);
		at = appendEscaped(at + 3, _pass);
		_cmdBuf[at++] = '"';
		_cmdLen = at;
		issue(Cmd::Join, espat_join_timeout_ms, nowMs);
		return 0;
	}
	if (_needAddr) {
		_needAddr = false;
		_q = NetConfig();
		formatCmd("AT+CIPSTA?");
		issue(Cmd::QueryIp, espat_cmd_timeout_ms, nowMs);
		return 0;
	}
	if (_needSto) {
		_needSto = false;
		formatCmd("AT+CIPSTO=0");	// server connections never time out on the module's side
		issue(Cmd::ServerTimeout, espat_cmd_timeout_ms, nowMs);
		return 0;
	}

	// Connects and listens, once there is an address.
	if (!_net.ip.isZero()) {
		for (uint8_t s = 0; s < espat_links; ++s) {
			Sock &k = _sock[s];
			if (k.req != Sock::ReqConnect && k.req != Sock::ReqListen) continue;
			const typename Sock::Req req = k.req;
			const IpAddress ip = k.ip;
			const uint16_t port = k.port;
			if (k.link >= 0) {	// drop what it had, quietly
				_stray |= static_cast<uint8_t>(1u << k.link);
				_linkOwner[k.link] = -1;
			}
			k = Sock();
			k.ip = ip;
			k.port = port;
			_cmdSock = static_cast<int8_t>(s);
			if (req == Sock::ReqListen) {
				if (_serverOn) {
					if (_serverPort != port) {	// the module has one server port
						emit(s, SocketEvent::Failed);
					} else {
						k.mode = Sock::Listening;
						emit(s, SocketEvent::Listening);
					}
					return 0;
				}
				k.mode = Sock::Listening;
				formatCmd("AT+CIPSERVER=1,%u", port);
				issue(Cmd::Server, espat_cmd_timeout_ms, nowMs);
				return 0;
			}
			const int8_t l = freeLink();
			if (l < 0) {
				emit(s, SocketEvent::Failed);
				return 0;
			}
			k.mode = Sock::Connecting;
			k.link = l;
			_linkOwner[l] = static_cast<int8_t>(s);
			_cmdLink = l;
			formatCmd("AT+CIPSTART=%d,\"TCP\",\"%u.%u.%u.%u\",%u", l, ip.b[0], ip.b[1], ip.b[2], ip.b[3], port);
			issue(Cmd::Start, espat_connect_timeout_ms, nowMs);
			return 0;
		}
	}

	// DNS and time, once there is an address.
	if (!_net.ip.isZero()) {
		if (_dnsReq) {
			_dnsReq = false;
			_dnsBusy = true;
			_dnsCmdGen = _dnsGen;
			_dnsResult = IpAddress();
			memcpy(_cmdBuf, "AT+CIPDOMAIN=\"", 14);
			size_t at = appendEscaped(14, _dnsName);
			_cmdBuf[at++] = '"';
			_cmdLen = at;
			issue(Cmd::Domain, espat_dns_timeout_ms, nowMs);
			return 0;
		}
		if (_ntpReq) {
			_ntpReq = false;
			_ntpBusy = true;
			if (strcmp(_ntpServer, _ntpConfigured) == 0) {
				_ntpPolling = true;	// the module already syncs with it: just read its time
				_ntpPollStart = nowMs;
				_ntpLastPoll = nowMs - espat_sntp_poll_ms;
			} else {
				memcpy(_cmdBuf, "AT+CIPSNTPCFG=1,0,\"", 19);	// on, UTC, this server
				size_t at = appendEscaped(19, _ntpServer);
				_cmdBuf[at++] = '"';
				_cmdLen = at;
				issue(Cmd::SntpCfg, espat_cmd_timeout_ms, nowMs);
				return 0;
			}
		}
		if (_ntpPolling && (nowMs - _ntpLastPoll) >= espat_sntp_poll_ms) {
			if ((nowMs - _ntpPollStart) >= espat_sntp_wait_ms) {
				_ntpPolling = false;
				_ntpBusy = false;
				if (_host) _host->timeReceived(false, 0, nowMs);	// never synchronised
				return 0;
			}
			_ntpLastPoll = nowMs;
			_ntpSec = 0;
			formatCmd("AT+CIPSNTPTIME?");
			issue(Cmd::SntpTime, espat_cmd_timeout_ms, nowMs);
			return 0;
		}
	}

	// Received data, if the reader has room for it.
	for (uint8_t s = 0; s < espat_links; ++s) {
		Sock &k = _sock[s];
		if (!k.rxPending || k.link < 0 || k.mode != Sock::Established || !_host) continue;
		size_t n = _host->rxSpace(s);
		if (n == 0) continue;	// the reader's kick brings us back
		if (n > espat_chunk_bytes) n = espat_chunk_bytes;
		_xLen = static_cast<uint16_t>(n);
		_binGot = 0;
		_cmdSock = static_cast<int8_t>(s);
		_cmdLink = k.link;
		formatCmd("AT+CIPRECVDATA=%d,%u", k.link, static_cast<unsigned>(n));
		issue(Cmd::RecvData, espat_cmd_timeout_ms, nowMs);
		return 0;
	}

	// Data to send.
	for (uint8_t s = 0; s < espat_links; ++s) {
		Sock &k = _sock[s];
		if (k.mode != Sock::Established || k.peerClosed || k.link < 0 || !_host || _host->txPending(s) == 0) continue;
		_xLen = static_cast<uint16_t>(_host->txTake(s, _chunk, espat_chunk_bytes));
		if (_xLen == 0) continue;
		_sendStage = 0;
		_cmdSock = static_cast<int8_t>(s);
		_cmdLink = k.link;
		formatCmd("AT+CIPSEND=%d,%u", k.link, _xLen);
		issue(Cmd::Send, espat_send_timeout_ms, nowMs);
		return 0;
	}

	// Closed by the peer, and everything it sent delivered.
	for (uint8_t s = 0; s < espat_links; ++s) {
		Sock &k = _sock[s];
		if (k.mode == Sock::Established && k.peerClosed && !k.rxPending) {
			closeSock(s, SocketEvent::Closed);
			return 0;
		}
	}

	// Still joined? And how well.
	if (_link && (nowMs - _lastAp) >= _param.apPollMs) {
		_lastAp = nowMs;
		formatCmd("AT+CWJAP?");
		issue(Cmd::QueryAp, espat_cmd_timeout_ms, nowMs);
		return 0;
	}

	uint32_t wait = 1000;
	if (_link) {
		const uint32_t w = remaining(nowMs, _lastAp, _param.apPollMs);
		if (w < wait) wait = w;
	}
	if (_ntpPolling) {
		const uint32_t w = remaining(nowMs, _ntpLastPoll, espat_sntp_poll_ms);
		if (w < wait) wait = w;
	}
	return wait == 0 ? 1 : wait;
}

// Gives up on the module: every connection is lost, and it starts
// again from a reset after a pause.
template <typename TTransport>
uint32_t espat<TTransport>::fail(uint32_t nowMs) {
	_stFailures.fetch_add(1, std::memory_order_relaxed);
	failQueries();
	for (uint8_t s = 0; s < espat_links; ++s) {
		Sock &k = _sock[s];
		const bool open = k.mode != Sock::Closed;
		const bool opening = k.req == Sock::ReqConnect || k.req == Sock::ReqListen;
		const bool closing = k.req == Sock::ReqClose;
		k = Sock();
		if (open || opening) emit(s, SocketEvent::Failed);
		else if (closing) emit(s, SocketEvent::Closed);
	}
	for (uint8_t l = 0; l < espat_links; ++l) _linkOwner[l] = -1;
	linkLost();
	_stray = 0;
	_serverOn = false;
	_needSto = false;
	_needAddr = false;
	_ready = false;
	_cmd = Cmd::None;
	_binLeft = 0;
	_lineLen = 0;
	emitDev(DeviceEvent::Failed);
	enter(St::error, nowMs);
	return 0;
}

// The module is restarting: whoever asked a question hears now rather
// than at their timeout. The module forgets its SNTP setting too.
template <typename TTransport>
void espat<TTransport>::failQueries() {
	if (_dnsReq || _dnsBusy) {
		_dnsReq = false;
		_dnsBusy = false;
		if (_host) _host->resolved(false, IpAddress());
	}
	if (_ntpReq || _ntpBusy) {
		_ntpReq = false;
		_ntpBusy = false;
		_ntpPolling = false;
		if (_host) _host->timeReceived(false, 0, 0);
	}
	_ntpConfigured[0] = 0;
}

} /* namespace ESPAT */

#endif /* ESPAT_TPP_ */
