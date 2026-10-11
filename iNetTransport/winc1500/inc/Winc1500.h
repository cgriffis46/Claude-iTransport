/*
 * Winc1500.h
 *
 *  Microchip (Atmel) ATWINC1500 Wi-Fi module as a Wi-Fi interface, over
 *  SPI. Non-blocking state machine, written for this repository against
 *  Microchip's host driver 19.5.2 as the protocol reference (see
 *  WincProtocol.h); it doesn't use that driver.
 *
 *  The bus is chosen by the template argument, as for the other
 *  drivers. winc1500<TTransport> inherits from TTransport, which must be
 *  an iBlockTransport (itransport/inc/iBlockTransport.h):
 *
 *      WINC1500::winc1500<Stm32HalSpiBlockTransport> winc(param, &hspi1, WINC_CS_GPIO_Port, WINC_CS_Pin, spi1Mutex);
 *      xWifi wifi(winc);
 *
 *  It is an iWifiDevice (iNetTransport/inc/iNetDevice.h): hand it to an
 *  xWifi. The module does TCP, DHCP, DNS and SNTP itself; this driver
 *  speaks its host interface.
 *
 *  How it talks to the module:
 *   - every register or memory access is a few SPI transfers: the
 *     command, its echo and status a byte at a time, then the data, as
 *     Microchip's driver does on real boards (chip-select rises between
 *     them). poll() never waits for one; each transfer is one state;
 *   - after a reset the SPI runs with CRC-7 on commands; the driver
 *     turns the CRC off, as Microchip's does;
 *   - requests go to the module as HIF messages in a buffer the module
 *     hands out; the module's messages (replies, events, received data)
 *     wait in its memory until the driver says "RX done". The IRQN pin
 *     (falling edge) says one is there: relay it with
 *     xNetInterface::interruptFromIsr(). Without it, set pollMs short;
 *   - received data: one receive request at a time per socket. A reply
 *     (up to 1400 bytes) is copied into the driver's buffer, the module
 *     released at once, and the bytes handed to the reader as it has
 *     room; the next receive on that socket is asked for only once they
 *     have all gone. A reader that has fallen behind by more than its
 *     buffer keeps the driver's buffer: a reply for another socket then
 *     waits in the module (and the module's other messages with it)
 *     until that reader catches up. Give sockets that may be read slowly
 *     an rx buffer of 1400 bytes or more (xNetInterface::Config::rxBufBytes);
 *   - sending: one send in flight per socket, up to 1400 bytes, the
 *     next one after the module's reply.
 *
 *  Firmware 19.5.0 or later (19.5.x and 19.6.1 are what the reference
 *  covers); older firmware is refused (Error::FirmwareTooOld): update it
 *  with Microchip's or Arduino's WiFi101 firmware updater. The driver
 *  reports itself as 19.5.2.
 *
 *  Sockets: the module has 7 TCP sockets. The interface gets
 *  winc_param_t::sockets of them (6 by default), leaving one for a
 *  listening socket: the module accepts a connection on a socket of its
 *  own and keeps the listening one, so a listen() here holds a module
 *  socket per port (shared by every interface socket listening on that
 *  port) and each accepted connection another. A listening socket nobody
 *  listens on any more is closed after a couple of seconds; a peer that
 *  arrives while nobody listens is closed.
 *
 *  The module doesn't rejoin by itself. When the access point goes away
 *  (or the module restarts), the driver joins the same network again
 *  every 5 s until leave() or another join(); those tries report nothing
 *  until LinkUp.
 *
 *  Not here: TLS on the module, UDP, power save, access point and
 *  provisioning modes, scanning, WPS, enterprise (802.1X) and WEP
 *  networks. requestTime() ignores the server named: the firmware's SNTP
 *  client uses its own servers (the time is good to about a second).
 *  connect()'s localPort is ignored: the module picks it.
 */

#ifndef WINC1500_H_
#define WINC1500_H_

#include <stddef.h>
#include <stdint.h>
#include <atomic>
#include <type_traits>
#include <utility>
#include "iBlockTransport.h"
#include "iNetDevice.h"
#include "WincProtocol.h"

namespace WINC1500 {

// All times in ms.
const uint32_t winc_bus_timeout_ms		= 100;		// one SPI transfer, to issue or to land
const uint32_t winc_reset_hold_ms		= 100;		// RESET_N low (Microchip's BSP: 100 ms each way)
const uint32_t winc_reset_wait_ms		= 100;		// after RESET_N goes high
const uint32_t winc_soft_reset_wait_ms	= 50;		// after a global reset by register (no reset pin)
const uint32_t winc_boot_timeout_ms		= 2000;		// efuses, then the boot ROM
const uint32_t winc_firmware_timeout_ms	= 4000;		// the firmware's "init done"
const uint32_t winc_alloc_timeout_ms	= 200;		// the module handing out a message buffer
const uint32_t winc_alloc_backoff_ms	= 20;		// then try that request again after
const uint32_t winc_error_backoff_ms	= 1000;
const uint32_t winc_join_timeout_ms		= 30000;	// CONNECT to "connected"
const uint32_t winc_dhcp_timeout_ms		= 30000;	// "connected" to an address
const uint32_t winc_connect_timeout_ms	= 30000;	// backstop: the module's own TCP timeout comes first
const uint32_t winc_listen_timeout_ms	= 5000;		// BIND/LISTEN replies
const uint32_t winc_send_timeout_ms		= 30000;	// a SEND's reply
const uint32_t winc_dns_timeout_ms		= 15000;
const uint32_t winc_sntp_wait_ms		= 15000;	// for the module's first SNTP sync
const uint32_t winc_sntp_poll_ms		= 1000;		// GET_SYS_TIME while waiting
const uint32_t winc_listen_linger_ms	= 2000;		// a listening socket nobody wants, before it closes
const uint32_t winc_rejoin_ms			= 5000;		// joining again after the network was lost
const uint8_t  winc_alloc_fails_max		= 25;		// requests in a row with no buffer (about 5 s): restart the module

// Received data: the most taken from the module at a time. RAM: the
// driver keeps one buffer this size, and one of
// winc_tx_bytes for the messages it sends.
const uint16_t winc_rx_bytes = 1400;
const uint16_t winc_tx_bytes = WINC::hif_header_bytes + WINC::send_data_offset + WINC::socket_max_send;

const uint8_t winc_listeners = 2;	// ports listened on at once

typedef struct winc_param_t {
	// Drives the module's RESET_N pin: true holds it in reset. With it,
	// every start begins with a hardware reset (100 ms low, then 100 ms);
	// without it (nullptr), with a global reset through a register. Tie
	// CHIP_EN and WAKE high.
	void (*hardReset)(bool asserted) = nullptr;
	// How often the module is asked whether it has a message for the
	// host, when nothing else prompts it. With IRQN wired to
	// xNetInterface::interruptFromIsr() this is only a backstop and can
	// be long (100 ms); without it, it is the receive latency.
	uint32_t pollMs = 5;
	// While joined: how often to ask for the signal strength (rssi()).
	uint32_t rssiPollMs = 10000;
	// Interface sockets, 1..7. Each listening port takes one more of the
	// module's 7 TCP sockets.
	uint8_t sockets = 6;
} winc_param_t;

// Counters, for bring-up and field diagnostics. stats() may be called
// from any thread.
typedef struct winc_stats_t {
	uint32_t transfers;		// SPI transfers issued
	uint32_t spiRetries;	// accesses repeated after an SPI reset
	uint32_t failures;		// times the driver gave up on the module and started again
	uint32_t inits;			// successful start-ups
	uint32_t interrupts;	// IRQN events relayed by the host
	uint32_t messagesIn;	// HIF messages from the module
	uint32_t messagesOut;	// HIF messages to the module
	uint32_t allocWaits;	// requests put off: the module had no buffer
	uint32_t heldReplies;	// received data left waiting in the module: the buffer was in use
	uint32_t rxBytes;		// TCP payload, module -> host
	uint32_t txBytes;		// TCP payload, host -> module
} winc_stats_t;

enum class winc_error_t : uint8_t {
	none,
	no_chip,			// no answer on the SPI bus, or a broken one
	wrong_chip,			// not an ATWINC1500 (chip ID)
	boot_timeout,		// efuses or the boot ROM never ready
	firmware_timeout,	// the firmware never said it was up
	firmware_too_old,	// before 19.5.0 (update it)
	firmware_too_new,	// it needs a newer host driver than 19.5.2
	bus,				// accesses failed after every retry
};

struct winc_version_t {
	uint8_t major, minor, patch;
};

enum class winc_state_t : uint8_t {
	unconfigured,
	reset_hold, reset_wait,
	spi_probe, spi_crc_off, soft_reset, soft_reset_wait,
	chip_id, pkt_size_read, pkt_size_write,
	efuse_wait, wait_host, bootrom_wait,
	host_version, conf_write, conf_check, start_firmware,
	firmware_wait, firmware_ack,
	irq_mux_read, irq_mux_write, irq_en_read, irq_en_write,
	gp2_read, gpregs_read, rev_read, mac_read,
	run,
	error
};

template <typename TTransport>
class winc1500 : protected TTransport, public iWifiDevice {
	static_assert(std::is_base_of<iBlockTransport, TTransport>::value,
		"winc1500<TTransport> needs an iBlockTransport (SPI: Stm32HalSpiBlockTransport, ...)");

public:
	template <typename... TArgs>
	explicit winc1500(const winc_param_t &param, TArgs&&... transportArgs);

	// iNetDevice
	void attach(iNetDeviceHost &host) override { _host = &host; }
	uint8_t socketCount() const override { return _nSocks; }
	void configure(const NetConfig &cfg) override;
	uint32_t poll(uint32_t nowMs) override;
	bool connect(uint8_t s, const IpAddress &ip, uint16_t port, uint16_t localPort) override;
	bool listen(uint8_t s, uint16_t port) override;
	void close(uint8_t s) override;
	void interrupt() override { _irq = true; _stats.interrupts++; }
	bool resolve(const char *name) override;
	bool requestTime(const char *server) override;

	// iWifiDevice
	void join(const char *ssid, const char *passphrase) override;
	void leave() override;
	int8_t rssi() const override { return _up ? _rssi : 0; }

	winc_state_t state() const { return _state; }
	winc_error_t error() const { return _error; }
	// From the module, once it has started (all zeros before).
	winc_version_t firmware() const { return _fw; }
	uint32_t chipId() const { return _chipId; }
	MacAddress mac() const { return _mac; }
	// The reason the last join failed (WINC::err_*), 0 if none.
	uint8_t joinError() const { return _joinErr; }

	winc_stats_t stats() const {
		winc_stats_t s;
		s.transfers = _stats.transfers.load();
		s.spiRetries = _stats.spiRetries.load();
		s.failures = _stats.failures.load();
		s.inits = _stats.inits.load();
		s.interrupts = _stats.interrupts.load();
		s.messagesIn = _stats.messagesIn.load();
		s.messagesOut = _stats.messagesOut.load();
		s.allocWaits = _stats.allocWaits.load();
		s.heldReplies = _stats.heldReplies.load();
		s.rxBytes = _stats.rxBytes.load();
		s.txBytes = _stats.txBytes.load();
		return s;
	}

private:
	// ---- the SPI access engine ----
	enum class OpKind : uint8_t { none, reg_read, reg_write, block_read, block_write };
	enum class OpResult : uint8_t { busy, done, failed };
	struct Op {
		OpKind kind = OpKind::none;
		uint8_t step = 0;
		uint8_t polls = 0;
		uint8_t tries = 0;
		bool xfer = false;			// a transfer in flight
		bool resetting = false;		// in the reset sequence before a retry
		uint32_t xferAt = 0;
		uint32_t delayUntil = 0;
		bool delaying = false;
		uint32_t addr = 0;
		uint32_t value = 0;
		uint8_t *rbuf = nullptr;
		const uint8_t *wbuf = nullptr;
		uint16_t len = 0;
	};
	void startOp(OpKind k, uint32_t addr, uint32_t value, uint8_t *rbuf, const uint8_t *wbuf, uint16_t len);
	void readReg(uint32_t addr) { startOp(OpKind::reg_read, addr, 0, nullptr, nullptr, 4); }
	void writeReg(uint32_t addr, uint32_t v) { startOp(OpKind::reg_write, addr, v, nullptr, nullptr, 4); }
	void readBlock(uint32_t addr, uint8_t *buf, uint16_t n) { startOp(OpKind::block_read, addr, 0, buf, nullptr, n); }
	void writeBlock(uint32_t addr, const uint8_t *buf, uint16_t n) { startOp(OpKind::block_write, addr, 0, nullptr, buf, n); }
	OpResult runOp(uint32_t now);
	bool opStep(uint32_t now, bool &again);
	bool opError(uint32_t now);
	bool resetStep(uint32_t now, bool &again);
	bool issueWrite(const uint8_t *hdr, uint8_t hl, const uint8_t *data, size_t n, uint32_t now);
	bool issueRead(uint8_t *data, size_t n, uint32_t now);
	uint8_t buildCommand();

	// ---- the start-up ----
	uint32_t runStartup(uint32_t now);
	void enter(winc_state_t s, uint32_t now) { _state = s; _phaseStart = now; _opActive = false; }
	void waitThen(uint32_t now, uint32_t ms) { _opActive = false; _delayUntil = now + ms; _delaying = true; }
	void fail(winc_error_t e, uint32_t now);
	void restart(uint32_t now);
	void resetRuntime();

	// ---- the running module ----
	uint32_t runMain(uint32_t now);
	uint32_t stepReceive(uint32_t now);
	uint32_t stepSend(uint32_t now);
	uint32_t stepHeldData(uint32_t now);
	uint32_t stepRxDone(uint32_t now);
	void progress(uint32_t now);
	bool prepareRequest(uint32_t now);
	void beginMessage(uint8_t gid, uint8_t op, uint16_t ctrlLen);
	void onSent(uint32_t now);
	void dispatch(uint32_t now);
	void wifiMessage(uint8_t op, const uint8_t *p, uint16_t n, uint32_t now);
	void ipMessage(uint8_t op, const uint8_t *p, uint16_t n, uint32_t now);
	void deliverRx();
	void checkTimers(uint32_t now);
	uint32_t nextWake(uint32_t now);
	void linkLost(uint32_t now);
	void dropSockets(uint32_t now);
	void timeReply(const uint8_t *p, uint32_t now);

	// Sockets.
	struct Sock {
		enum Mode : uint8_t { Idle, Connecting, Listening, Established };
		enum Req : uint8_t { NoReq, ConnectReq, ListenReq };
		Mode mode = Idle;
		Req req = NoReq;
		int8_t w = -1;				// the module's socket
		uint16_t session = 0;
		IpAddress ip;
		uint16_t port = 0;
		bool sent = false;			// CONNECT sent, waiting for the reply
		bool reported = false;		// Listening reported
		bool recvArmed = false;
		bool sendInFlight = false;
		uint16_t sendLen = 0;
		bool closedEvent = false;	// a Closed to report (close())
		bool ending = false;		// the peer closed: report it once the reader has the data
		SocketEvent endEv = SocketEvent::Closed;
		uint32_t since = 0;
	};
	struct Listener {
		enum St : uint8_t { Free, NeedBind, BindSent, NeedListen, ListenSent, Open };
		St st = Free;
		uint16_t port = 0;
		int8_t w = -1;
		uint16_t session = 0;
		uint32_t since = 0;
		bool idle = false;
		uint32_t idleSince = 0;
	};
	static const uint8_t kFreeW = 0xFF;
	static const uint8_t kStrayW = 0xFE;			// closing, or an accepted peer nobody wants
	static const uint8_t kListenerW = 0x80;		// | listener index
	int8_t allocW(uint8_t owner);
	void queueClose(int8_t w);
	void socketFailed(uint8_t s, SocketEvent ev);
	void listenerFailed(uint8_t l);
	uint16_t nextSession() { if (++_session == 0) ++_session; return _session; }
	void emit(uint8_t s, SocketEvent ev) { if (_host) _host->socketEvent(s, ev); }
	void devEvent(DeviceEvent ev) { if (_host) _host->deviceEvent(ev); }

	winc_param_t _param;
	iNetDeviceHost *_host = nullptr;
	uint8_t _nSocks;

	winc_state_t _state = winc_state_t::unconfigured;
	winc_error_t _error = winc_error_t::none;
	bool _configured = false;
	NetConfig _cfg;
	NetConfig _net;			// the address in use
	uint32_t _phaseStart = 0;
	bool _delaying = false;
	uint32_t _delayUntil = 0;
	bool _softResetDone = false;
	bool _crcOff = false;
	bool _crcProbeOff = false;	// spi_probe: trying with the CRC off
	uint32_t _cfgVal = 0;		// a register value being built
	uint8_t _confTries = 0;

	// The access in progress.
	Op _op;
	bool _opActive = false;
	uint32_t _opValue = 0;
	uint8_t _cmd[10];
	uint8_t _b[4];				// single response bytes, a register's data
	uint8_t _crcBuf[2];
	uint8_t _tmp2[2];			// the one-byte workaround

	// Start-up results.
	uint32_t _chipId = 0;
	winc_version_t _fw = {0, 0, 0};
	MacAddress _mac;
	uint8_t _gp[8];
	uint8_t _rev[WINC::rev_bytes];
	uint8_t _macBuf[6];
	uint8_t _setupStep = 0;		// the set-up requests after start-up
	bool _ready = false;

	// HIF: what's being done now.
	enum class Proc : uint8_t { none, receive, send, held_data, rx_done };
	Proc _proc = Proc::none;
	uint8_t _pstep = 0;
	bool _irq = false;
	uint32_t _nextCheck = 0;
	uint32_t _sendBackoffUntil = 0;

	// The message coming in.
	uint8_t _hdr[WINC::hif_header_bytes + WINC::max_reply_bytes];
	uint32_t _msgAddr = 0;
	uint16_t _msgSize = 0;
	uint32_t _ctrl0 = 0;
	bool _msgHeld = false;		// not "RX done" yet
	// A RECV reply's data still in the module.
	int8_t _heldSock = -1;		// the interface socket it's for (-1: drop it)
	uint32_t _heldAddr = 0;
	uint16_t _heldLeft = 0;
	bool _heldWaitsBuffer = false;
	bool _heldCounted = false;
	uint16_t _heldPiece = 0;

	// The message going out.
	uint8_t _tx[winc_tx_bytes];
	uint16_t _txLen = 0;
	bool _txPending = false;	// built, not yet taken by the module
	uint8_t _txGid = 0, _txOp = 0;
	uint32_t _allocAt = 0;
	uint32_t _txDma = 0;
	enum class TxFor : uint8_t { none, setup, leave, join, close, bind, listen, connect, send, recv, dns, time, rssi };
	TxFor _txFor = TxFor::none;
	int8_t _txIdx = -1;

	// Received data on its way to a reader.
	uint8_t _rx[winc_rx_bytes];
	uint16_t _rxLen = 0, _rxPos = 0;
	int8_t _rxOwner = -1;

	// Wi-Fi.
	char _ssid[WINC::max_ssid_len + 1];
	char _pass[WINC::max_psk_len + 1];
	bool _joinReq = false;		// a CONNECT to send
	bool _joining = false;		// sent, not yet up
	bool _leaveReq = false;
	bool _leaving = false;
	bool _leaveQuiet = false;	// leaving to join another network: no LinkDown of its own
	bool _rejoin = false;		// joining again by ourselves: a failure isn't reported
	uint32_t _rejoinAt = 0;
	uint8_t _allocFails = 0;
	uint32_t _leaveAt = 0;
	bool _assoc = false;		// associated with the access point
	bool _up = false;			// associated, with an address
	uint8_t _joinErr = 0;
	uint32_t _joinAt = 0;
	int8_t _rssi = 0;
	uint32_t _nextRssi = 0;

	// Sockets.
	Sock _sock[WINC::tcp_sockets];
	Listener _lst[winc_listeners];
	uint8_t _wOwner[WINC::tcp_sockets];
	uint16_t _wSession[WINC::tcp_sockets];
	uint8_t _closeMask = 0;		// module sockets to send CLOSE for
	uint16_t _session = 0;
	uint8_t _sendRr = 0, _recvRr = 0;

	// DNS and time.
	char _dnsName[WINC::hostname_max];
	bool _dnsReq = false, _dnsSent = false;
	uint32_t _dnsAt = 0;
	bool _timeReq = false, _timeSent = false;
	uint32_t _timeAt = 0, _timeNext = 0;

	struct Stats {
		std::atomic<uint32_t> transfers{0}, spiRetries{0}, failures{0}, inits{0}, interrupts{0},
			messagesIn{0}, messagesOut{0}, allocWaits{0}, heldReplies{0}, rxBytes{0}, txBytes{0};
	} _stats;
};

}  // namespace WINC1500

#include "../src/Winc1500.tpp"

#endif /* WINC1500_H_ */
