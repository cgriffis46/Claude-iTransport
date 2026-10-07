/*
 * EspAt.h
 *
 *  An Espressif module running ESP-AT firmware (ESP32, ESP32-C3,
 *  ESP8266) as a Wi-Fi interface, over a UART. Non-blocking state
 *  machine.
 *
 *  The bus is chosen by the template argument, as for the other
 *  drivers. espat<TTransport> inherits from TTransport, which must be
 *  an iTransport (itransport/inc/iTransport.h) — a byte stream that
 *  pushes what it receives to a sink from its ISR:
 *
 *      ESPAT::espat<Stm32HalUartTransport> esp(param, &huart1);
 *      xWifi wifi(esp);
 *
 *  It is an iWifiDevice (inet/inc/iNetDevice.h): hand it to an xWifi.
 *  The module does TCP and DHCP itself; this driver speaks AT to it.
 *
 *  How it talks to the module:
 *   - one AT command at a time; poll() never waits for the answer, it
 *     returns, and the UART ISR wakes the driver thread when a line
 *     ends (iNetDeviceHost::wakeFromIsr());
 *   - received bytes go from the ISR into a lock-free ring, and are
 *     parsed into lines on the driver thread;
 *   - multiple connections (AT+CIPMUX=1), link IDs 0..4;
 *   - received data in passive mode (AT+CIPRECVMODE=1): the module
 *     holds it and says "+IPD,<link>,<len>", and the driver fetches it
 *     with AT+CIPRECVDATA only when the reader has room. A slow reader
 *     backs up into the module's buffer and then TCP's window, never
 *     into lost bytes;
 *   - sending with AT+CIPSEND=<link>,<len>, ">", the data, "SEND OK".
 *
 *  Written against ESP-AT v2.x (ESP32 family); the commands used are
 *  also in ESP8266 AT 1.7, and both forms of the +CIPRECVDATA reply
 *  are understood. Listening: the module has one server port, so every
 *  xClient that listens must use the same port.
 *
 *  Throughput is the UART's: about 11 KB/s each way at 115200 baud.
 *  Raise the baud rate with AT+UART_DEF once, by hand, if you need more.
 */

#ifndef ESPAT_H_
#define ESPAT_H_

#include <stddef.h>
#include <stdint.h>
#include <atomic>
#include <type_traits>
#include <utility>
#include "iTransport.h"
#include "iNetDevice.h"

namespace ESPAT {

const uint8_t  espat_links = 5;				// link IDs 0..4, with AT+CIPMUX=1
const uint16_t espat_chunk_bytes = 1024;	// most data moved per AT+CIPSEND / AT+CIPRECVDATA
const uint16_t espat_ring_bytes = 2048;		// receive ring, a power of two: the UART's slack
const uint8_t  espat_line_max = 160;		// longest line kept; the rest is cut off

// All times in ms.
const uint32_t espat_cmd_timeout_ms		= 3000;
const uint32_t espat_join_timeout_ms	= 25000;	// AT+CWJAP; the module gives up after 15 s by default
const uint32_t espat_connect_timeout_ms	= 15000;	// AT+CIPSTART
const uint32_t espat_send_timeout_ms	= 10000;	// AT+CIPSEND, through to SEND OK
const uint32_t espat_ready_timeout_ms	= 5000;		// "ready" after a reset
const uint32_t espat_probe_interval_ms	= 500;		// "AT" until the module answers
const uint8_t  espat_probe_tries		= 10;
const uint32_t espat_busy_retry_ms		= 100;		// after "busy p..."
const uint8_t  espat_busy_tries			= 50;
const uint32_t espat_error_backoff_ms	= 1000;

typedef struct espat_param_t {
	// Drives the module's EN (or RST) pin: true holds it in reset.
	// With it, every start begins with a clean hardware reset; without
	// it (nullptr), with AT+RST.
	void (*hardReset)(bool asserted) = nullptr;
	// While joined: how often to ask the module about the access point
	// (AT+CWJAP?), which refreshes rssi() and checks it still answers.
	uint32_t apPollMs = 10000;
} espat_param_t;

// Counters, for bring-up and field diagnostics. stats() may be called
// from any thread.
typedef struct espat_stats_t {
	uint32_t commands;		// AT commands sent
	uint32_t timeouts;		// commands the module never answered
	uint32_t failures;		// times the driver gave up on the module and started again
	uint32_t rxOverflows;	// UART bytes lost: the receive ring was full
	uint32_t rxBytes;		// TCP payload, module -> host
	uint32_t txBytes;		// TCP payload, host -> module
} espat_stats_t;

enum class espat_state_t : uint8_t {
	unconfigured,
	reset_hold,		// EN low
	wait_ready,		// "ready" after a reset
	probe,			// "AT" until "OK"
	init,			// the set-up commands, one after another
	idle,
	wait,			// a command in flight
	busy_wait,		// the module said "busy": send it again shortly
	error
};

template <typename TTransport>
class espat : protected TTransport, public iWifiDevice, private iTransportRxSink {
	static_assert(std::is_base_of<iTransport, TTransport>::value,
		"espat<TTransport> needs an iTransport (a UART: Stm32HalUartTransport, ...)");

public:
	template <typename... TArgs>
	explicit espat(const espat_param_t &param, TArgs&&... transportArgs);

	// iNetDevice
	void attach(iNetDeviceHost &host) override { _host = &host; }
	uint8_t socketCount() const override { return espat_links; }
	void configure(const NetConfig &cfg) override;
	uint32_t poll(uint32_t nowMs) override;
	bool connect(uint8_t s, const IpAddress &ip, uint16_t port, uint16_t localPort) override;
	bool listen(uint8_t s, uint16_t port) override;
	void close(uint8_t s) override;
	void interrupt() override {}

	// iWifiDevice
	void join(const char *ssid, const char *passphrase) override;
	void leave() override;
	int8_t rssi() const override { return _link ? _rssi : 0; }

	espat_state_t state() const { return _state; }
	bool ready() const { return _ready; }
	bool linkUp() const { return _link; }
	const NetConfig &address() const { return _net; }

	// The module's "AT version:..." line from AT+GMR, once ready().
	// Written before DeviceEvent::Ready, and not again until the next
	// start, so reading it after xNetInterface::waitReady() is safe.
	const char *firmware() const { return _firmware; }

	espat_stats_t stats() const {
		espat_stats_t s;
		s.commands = _stCommands.load(std::memory_order_relaxed);
		s.timeouts = _stTimeouts.load(std::memory_order_relaxed);
		s.failures = _stFailures.load(std::memory_order_relaxed);
		s.rxOverflows = _stOverflows.load(std::memory_order_relaxed);
		s.rxBytes = _stRx.load(std::memory_order_relaxed);
		s.txBytes = _stTx.load(std::memory_order_relaxed);
		return s;
	}

private:
	enum class Cmd : uint8_t {
		None, Probe, Rst, Init, Join, Leave, QueryIp, QueryDns, QueryAp,
		Start, Server, ServerTimeout, Close, RecvData, Send
	};
	enum class Res : uint8_t { Pending, Ok, Error, SendOk, Prompt, Busy, Timeout };

	struct Sock {
		enum Mode : uint8_t { Closed, Connecting, Listening, Established, Closing };
		enum Req : uint8_t { NoReq, ReqConnect, ReqListen, ReqClose };
		Mode      mode = Closed;
		Req       req = NoReq;
		IpAddress ip;
		uint16_t  port = 0;
		int8_t    link = -1;
		bool      rxPending = false;	// the module holds data for us
		bool      peerClosed = false;	// "<link>,CLOSED": deliver what's left, then Closed
	};

	// iTransportRxSink: the UART ISR.
	void onByteReceived(uint8_t byte) override;

	void drain(uint32_t nowMs);
	void feed(uint8_t c);
	void onLine(const char *line);
	void binaryDone();

	uint32_t step(uint32_t nowMs);
	uint32_t schedule(uint32_t nowMs);
	bool     issue(Cmd cmd, uint32_t timeoutMs, uint32_t nowMs);	// sends _cmdBuf
	void     finish(Res res, uint32_t nowMs);
	bool     nextInit(uint32_t nowMs);
	uint32_t fail(uint32_t nowMs);
	void     linkLost();
	void     closeSock(uint8_t s, SocketEvent ev);
	void     incoming(uint8_t link);
	void     linkClosed(uint8_t link);
	int8_t   freeLink() const;
	void     formatCmd(const char *fmt, ...);
	size_t   appendEscaped(size_t at, const char *s);

	void enter(espat_state_t next, uint32_t nowMs) { _state = next; _since = nowMs; }
	bool elapsed(uint32_t nowMs, uint32_t ms) const { return (nowMs - _since) >= ms; }
	static uint32_t remaining(uint32_t nowMs, uint32_t t0, uint32_t ms) {
		const uint32_t gone = nowMs - t0;
		return gone >= ms ? 0 : ms - gone;
	}
	void emit(uint8_t s, SocketEvent ev) { if (_host) _host->socketEvent(s, ev); }
	void emitDev(DeviceEvent ev) { if (_host) _host->deviceEvent(ev); }

	espat_param_t   _param;
	NetConfig       _cfg;
	NetConfig       _net;				// address in use
	NetConfig       _q;					// being read back from the module
	iNetDeviceHost *_host = nullptr;
	Sock            _sock[espat_links];
	int8_t          _linkOwner[espat_links] = {-1, -1, -1, -1, -1};
	uint8_t         _stray = 0;			// links to close that nobody wants (incoming, no listener)

	espat_state_t _state = espat_state_t::unconfigured;
	uint32_t      _since = 0;
	bool          _configured = false;
	bool          _reconfigure = false;
	bool          _ready = false;
	bool          _link = false;
	bool          _resetDone = false;		// this start has reset the module
	bool          _rebooted = false;		// "ready" out of the blue
	uint8_t       _tries = 0;
	uint8_t       _initIdx = 0;

	// Wi-Fi requests and state.
	bool     _joinReq = false;
	bool     _leaveReq = false;
	bool     _needAddr = false;			// read the address back from the module
	uint32_t _lastAp = 0;
	int8_t   _rssi = 0;
	char     _ssid[33] = {0};
	char     _pass[65] = {0};

	// Server.
	bool     _serverOn = false;
	uint16_t _serverPort = 0;
	bool     _needSto = false;			// AT+CIPSTO=0 after the server starts

	// The command in flight.
	Cmd      _cmd = Cmd::None;
	Res      _res = Res::Pending;
	uint32_t _cmdTimeout = 0;
	int8_t   _cmdSock = -1;
	int8_t   _cmdLink = -1;
	uint8_t  _sendStage = 0;			// AT+CIPSEND: 0 waiting for ">", 1 for SEND OK
	uint16_t _xLen = 0;					// bytes asked for (recv) or being sent
	uint8_t  _busyTries = 0;
	bool     _ipdDuring = false;		// "+IPD" for the link being fetched, while it was
	char     _cmdBuf[256] = {0};		// room for a fully escaped SSID and passphrase
	size_t   _cmdLen = 0;

	// Receive: ISR -> ring -> lines.
	uint8_t               _ring[espat_ring_bytes];
	std::atomic<uint16_t> _head{0};		// written by the ISR
	std::atomic<uint16_t> _tail{0};		// written by the driver thread
	std::atomic<bool>     _overflow{false};
	char     _firmware[64] = {0};
	std::atomic<uint32_t> _stCommands{0}, _stTimeouts{0}, _stFailures{0}, _stOverflows{0}, _stRx{0}, _stTx{0};
	char     _line[espat_line_max] = {0};
	uint8_t  _lineLen = 0;
	uint16_t _binLeft = 0;				// +CIPRECVDATA payload still to come
	uint16_t _binGot = 0;

	uint8_t  _chunk[espat_chunk_bytes];	// payload either way
};

} /* namespace ESPAT */

#include "../src/EspAt.tpp"

#endif /* ESPAT_H_ */
