/*
 * EspAt_test.cpp
 *
 * Host test for espat<TTransport> on its own: one thread, a simulated
 * clock, the simulated module in test/sim/SimEspAt.h, and a plain host
 * standing in for xWifi. No hardware, HAL or RTOS.
 *
 *   g++ -std=c++17 -Wall -Wextra -I../inc -I../../inc -I../../test/sim \
 *       -I<iTransport>/itransport/inc EspAt_test.cpp -o EspAt_test
 */

#include <cstdio>
#include <deque>
#include <memory>
#include <string>
#include <vector>
#include "EspAt.h"
#include "SimEspAt.h"

using namespace ESPAT;

static int g_failures = 0;
static void check(bool ok, const char *what) {
	std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok) ++g_failures;
}

struct Host : iNetDeviceHost {
	std::deque<uint8_t> rx[5], tx[5];
	size_t rxCap = 4096;
	bool overrun = false;
	int wakes = 0;
	std::vector<std::pair<int, SocketEvent>> events;
	std::vector<DeviceEvent> dev;
	std::vector<NetConfig> addrs;

	void wakeFromIsr() override { ++wakes; }
	size_t rxSpace(uint8_t s) override { return rxCap - rx[s].size(); }
	void rxDeliver(uint8_t s, const uint8_t *d, size_t n) override {
		if (n > rxSpace(s)) overrun = true;
		rx[s].insert(rx[s].end(), d, d + n);
	}
	size_t txPending(uint8_t s) override { return tx[s].size(); }
	size_t txTake(uint8_t s, uint8_t *d, size_t max) override {
		size_t n = 0;
		while (n < max && !tx[s].empty()) { d[n++] = tx[s].front(); tx[s].pop_front(); }
		return n;
	}
	void socketEvent(uint8_t s, SocketEvent e) override { events.push_back({s, e}); }
	void deviceEvent(DeviceEvent e) override { dev.push_back(e); }
	void addressChanged(const NetConfig &c) override { addrs.push_back(c); }
	int resolvedCount = 0, timeCount = 0;
	bool resolvedOk = false, timeOk = false;
	IpAddress resolvedIp;
	uint64_t timeUnixMs = 0;
	uint32_t timeAtMs = 0;
	void resolved(bool ok, const IpAddress &ip) override { ++resolvedCount; resolvedOk = ok; resolvedIp = ip; }
	void timeReceived(bool ok, uint64_t unixMs, uint32_t atMs) override {
		++timeCount; timeOk = ok; timeUnixMs = unixMs; timeAtMs = atMs;
	}

	bool has(int s, SocketEvent e) const { for (auto &p : events) if (p.first == s && p.second == e) return true; return false; }
	bool hasDev(DeviceEvent e) const { for (auto d : dev) if (d == e) return true; return false; }
	int countDev(DeviceEvent e) const { int n = 0; for (auto d : dev) n += d == e; return n; }
	std::vector<uint8_t> takeRx(int s) { std::vector<uint8_t> v(rx[s].begin(), rx[s].end()); rx[s].clear(); return v; }
};

typedef espat<FakeEspUart> Esp;

static std::vector<int> g_resetPin;
static void resetPin(bool asserted) { g_resetPin.push_back(asserted ? 1 : 0); }

struct Rig {
	SimEspAt sim;
	Host host;
	std::unique_ptr<Esp> esp;
	uint32_t now = 1000;
	NetConfig cfg;

	explicit Rig(const espat_param_t &p = espat_param_t()) {
		esp.reset(new Esp(p, sim));
		esp->attach(host);
		cfg.dhcp = true;
	}
	void run(uint32_t ms) {
		for (uint32_t i = 0; i < ms; ++i, ++now) {
			for (int j = 0; j < 20 && esp->poll(now) == 0; ++j) {}
		}
	}
	void start() { esp->configure(cfg); run(50); }
	void joinUp() { esp->join("home", "secret"); run(50); }
};

static std::vector<uint8_t> pattern(size_t n, uint8_t seed) {
	std::vector<uint8_t> v(n);
	for (size_t i = 0; i < n; ++i) v[i] = static_cast<uint8_t>(seed + i * 11 + (i >> 7));
	// The things a line parser could trip on, in the middle of the data.
	const char *nasty = "\r\nOK\r\n>+IPD,0,5\r\n0,CLOSED\r\nERROR\r\n";
	if (n > 200) std::memcpy(&v[100], nasty, std::strlen(nasty));
	return v;
}

int main() {
	std::printf("start up\n");
	{
		Rig r;
		r.run(20);
		check(r.sim.cmds.empty(), "nothing before configure()");
		r.start();
		check(r.esp->ready() && r.host.hasDev(DeviceEvent::Ready), "Ready");
		check(r.sim.resets == 1, "module reset with AT+RST");
		const char *want[] = {"AT", "AT+RST", "AT", "ATE0", "AT+GMR", "AT+CWMODE=1", "AT+CIPMUX=1", "AT+CIPRECVMODE=1",
		                      "AT+CWAUTOCONN=0", "AT+CWDHCP=1,1"};
		bool same = r.sim.cmds.size() == sizeof want / sizeof want[0];
		for (size_t i = 0; same && i < r.sim.cmds.size(); ++i) same = r.sim.cmds[i] == want[i];
		check(same, "AT, AT+RST, AT, ATE0, AT+GMR, station, multiple links, passive receive, no auto-join, DHCP");
		check(std::strncmp(r.esp->firmware(), "AT version:2.4.0.0", 18) == 0, "firmware version kept");
		check(r.esp->stats().commands == 10 && r.esp->stats().timeouts == 0, "stats(): commands counted");
		check(!r.esp->linkUp() && r.host.addrs.empty(), "not joined, no address");
	}

	std::printf("start up with a reset pin, and a static address\n");
	{
		espat_param_t p;
		p.hardReset = resetPin;
		Rig r(p);
		r.cfg.dhcp = false;
		r.cfg.ip = IpAddress(10, 1, 2, 3);
		r.cfg.gateway = IpAddress(10, 1, 2, 1);
		r.cfg.subnet = IpAddress(255, 255, 0, 0);
		r.esp->configure(r.cfg);
		r.run(30);
		check(g_resetPin.size() == 2 && g_resetPin[0] == 1 && g_resetPin[1] == 0, "EN pulsed low, then released");
		r.sim.reboot(); // what the module prints as it comes out of reset
		r.run(50);
		check(r.esp->ready() && r.sim.resets == 0, "Ready, without AT+RST");
		check(r.sim.sawCmd("AT+CIPSTA=\"10.1.2.3\",\"10.1.2.1\",\"255.255.0.0\""), "static address set with AT+CIPSTA");
	}

	std::printf("join\n");
	{
		Rig r;
		r.start();
		r.esp->join("home", "wrong");
		r.run(50);
		check(r.host.hasDev(DeviceEvent::JoinFailed) && !r.esp->linkUp(), "wrong passphrase: JoinFailed");
		r.joinUp();
		check(r.host.hasDev(DeviceEvent::LinkUp) && r.esp->linkUp(), "LinkUp");
		check(r.host.addrs.size() == 1, "address reported");
		const NetConfig &a = r.host.addrs.back();
		check(a.ip == r.sim.ip && a.gateway == r.sim.gateway && a.subnet == r.sim.netmask && a.dns == r.sim.dns && a.dhcp,
		      "read back from the module: AT+CIPSTA?, AT+CIPDNS?");
		r.run(10100);
		check(r.esp->rssi() == -61, "rssi from AT+CWJAP?, polled");

		r.sim.ssid = "cafe, \"the\" \\best";
		r.esp->join("cafe, \"the\" \\best", "secret");
		r.run(50);
		check(r.sim.joined() && r.host.countDev(DeviceEvent::JoinFailed) == 1, "SSID with , \" and \\ escaped properly");
		check(r.sim.lastCmdStarting("AT+CWJAP=") == "AT+CWJAP=\"cafe\\, \\\"the\\\" \\\\best\",\"secret\"", "as ESP-AT wants");

		r.esp->leave();
		r.run(50);
		check(!r.esp->linkUp() && r.host.dev.back() == DeviceEvent::LinkDown && r.host.addrs.back().ip.isZero(),
		      "leave(): LinkDown, address gone");
	}

	std::printf("connect, send, receive\n");
	{
		Rig r;
		r.host.rxCap = 700;	// smaller than a chunk: the driver must fetch only what fits
		r.start();
		r.esp->connect(0, IpAddress(93, 184, 216, 34), 80, 0);
		r.run(50);
		check(r.host.events.empty(), "connect() held until joined");
		r.joinUp();
		check(r.host.has(0, SocketEvent::Connected), "Connected");
		check(r.sim.lastCmdStarting("AT+CIPSTART") == "AT+CIPSTART=0,\"TCP\",\"93.184.216.34\",80", "AT+CIPSTART on link 0");

		const std::vector<uint8_t> up = pattern(3000, 1);
		r.host.tx[0].assign(up.begin(), up.end());
		r.run(50);
		check(r.sim.sent(0) == up, "3000 bytes sent intact in AT+CIPSEND pieces");

		const std::vector<uint8_t> down = pattern(3000, 2);
		r.sim.peerSend(0, down);
		std::vector<uint8_t> got;
		for (int i = 0; i < 20; ++i) {
			r.run(5);
			auto c = r.host.takeRx(0);
			got.insert(got.end(), c.begin(), c.end());
		}
		check(got == down, "3000 bytes received intact, \"\\r\\nOK\\r\\n\" and all in the payload");
		check(!r.host.overrun, "never more than rxSpace() at a time");

		r.host.rxCap = 0;
		r.sim.peerSend(0, std::vector<uint8_t>(10, 'x'));
		r.run(50);
		check(r.host.rx[0].empty(), "nothing fetched while the reader is full");
		r.host.rxCap = 700;
		r.run(5);
		check(r.host.rx[0].size() == 10, "fetched once there is room");
	}

	std::printf("ESP8266 AT 1.7 receive format\n");
	{
		Rig r;
		r.sim.v1RecvFormat = true;
		r.start();
		r.joinUp();
		r.esp->connect(2, IpAddress(1, 2, 3, 4), 80, 0);
		r.run(20);
		const std::vector<uint8_t> down = pattern(500, 3);
		r.sim.peerSend(0, down);	// first free link is 0
		r.run(20);
		check(r.host.takeRx(2) == down, "\"+CIPRECVDATA,<len>:<data>\" understood");
	}

	std::printf("closing\n");
	{
		Rig r;
		r.start();
		r.joinUp();
		r.esp->connect(1, IpAddress(1, 2, 3, 4), 80, 0);
		r.esp->connect(3, IpAddress(1, 2, 3, 5), 80, 0);
		r.run(20);
		r.esp->close(1);
		r.run(20);
		check(r.host.has(1, SocketEvent::Closed) && !r.sim.linkOpen(0), "close(): AT+CIPCLOSE, Closed");

		const std::vector<uint8_t> last = pattern(300, 4);
		r.sim.peerSend(1, last);	// socket 3 is on link 1
		r.sim.peerClose(1);
		r.run(20);
		check(r.host.takeRx(3) == last, "peer closed: what it sent first is still delivered");
		check(r.host.has(3, SocketEvent::Closed), "then Closed");
	}

	std::printf("listen\n");
	{
		Rig r;
		r.start();
		r.joinUp();
		r.esp->listen(4, 8080);
		r.run(20);
		check(r.sim.sawCmd("AT+CIPSERVER=1,8080") && r.sim.sawCmd("AT+CIPSTO=0"), "AT+CIPSERVER, no server timeout");
		check(r.host.has(4, SocketEvent::Listening), "Listening");
		const int l = r.sim.peerConnect(8080);
		r.run(20);
		check(r.host.has(4, SocketEvent::Connected), "Connected when a peer arrives");
		r.sim.peerSend(l, std::vector<uint8_t>{'h', 'i'});
		r.run(20);
		check(r.host.rx[4].size() == 2, "and its data arrives on that socket");
		const int stray = r.sim.peerConnect(8080);
		r.run(20);
		check(stray >= 0 && !r.sim.linkOpen(stray), "a second peer with nobody listening is closed");
		r.esp->listen(2, 9090);
		r.run(20);
		check(r.host.has(2, SocketEvent::Failed), "a second port can't be served: Failed");
	}

	std::printf("DNS\n");
	{
		Rig r;
		r.start();
		r.esp->resolve("pool.ntp.org");
		r.run(50);
		check(r.host.resolvedCount == 0, "held until joined");
		r.joinUp();
		check(r.host.resolvedCount == 1 && r.host.resolvedOk && r.host.resolvedIp == IpAddress(162, 159, 200, 1),
		      "AT+CIPDOMAIN answered");
		check(r.sim.lastCmdStarting("AT+CIPDOMAIN") == "AT+CIPDOMAIN=\"pool.ntp.org\"", "as ESP-AT wants");
		r.esp->resolve("nosuch.example");
		r.run(20);
		check(r.host.resolvedCount == 2 && !r.host.resolvedOk, "unknown name: not ok");
		check(!r.esp->resolve(std::string(65, 'a').c_str()), "over 64 characters: refused (the firmware's limit)");

		r.sim.hosts["b.example"] = IpAddress(10, 0, 0, 2);
		r.sim.holdDns = true;
		r.esp->resolve("pool.ntp.org");
		r.run(20);
		r.esp->resolve("b.example");	// while the first is still being looked up
		r.sim.holdDns = false;
		r.sim.releaseDns();
		r.run(20);
		check(r.host.resolvedCount == 3 && r.host.resolvedIp == IpAddress(10, 0, 0, 2),
		      "a second resolve() replaces the first: one answer, the second's");
	}

	std::printf("time\n");
	{
		Rig r;
		r.start();
		r.joinUp();
		check(r.esp->requestTime("pool.ntp.org"), "requestTime() taken");
		r.run(500);
		check(r.host.timeCount == 0, "not before the module has synchronised");
		r.run(3000);
		check(r.sim.sntpServer() == "pool.ntp.org", "AT+CIPSNTPCFG=1,0 (UTC) with the server");
		check(r.host.timeCount == 1 && r.host.timeOk && r.host.timeUnixMs == uint64_t(r.sim.unixSec) * 1000 + 500,
		      "AT+CIPSNTPTIME? polled until it isn't 1970, then read (to the second, +500 ms)");
		const size_t cmds = r.sim.cmds.size();
		r.esp->requestTime("pool.ntp.org");
		r.run(50);
		bool reconfigured = false;
		for (size_t i = cmds; i < r.sim.cmds.size(); ++i) reconfigured |= r.sim.cmds[i].compare(0, 13, "AT+CIPSNTPCFG") == 0;
		check(r.host.timeCount == 2 && r.host.timeOk && !reconfigured, "same server again: read at once, not reconfigured");

		r.sim.sntpSyncAfter = 1000;	// never synchronises
		r.esp->requestTime("time.example");
		r.run(16000);
		check(r.host.timeCount == 3 && !r.host.timeOk, "a module that never synchronises: not ok after 15 s");
	}

	std::printf("busy\n");
	{
		Rig r;
		r.start();
		r.sim.busyNext = 3;
		r.joinUp();
		r.run(500);
		check(r.esp->linkUp(), "\"busy p...\" three times: the command is sent again until it's taken");
	}

	std::printf("trouble\n");
	{
		Rig r;
		r.sim.silent = true;
		r.start();
		r.run(6000);
		check(!r.esp->ready() && r.host.hasDev(DeviceEvent::Failed), "no answer: Failed after the probes");
		r.sim.silent = false;
		r.run(8000);
		check(r.esp->ready(), "comes up once the module answers");

		r.joinUp();
		r.esp->connect(0, IpAddress(1, 2, 3, 4), 80, 0);
		r.run(20);
		r.host.events.clear();
		r.host.dev.clear();
		r.sim.dropWifi();
		r.run(20);
		check(r.host.has(0, SocketEvent::Failed) && r.host.hasDev(DeviceEvent::LinkDown) && r.host.addrs.back().ip.isZero(),
		      "WIFI DISCONNECT: connection Failed, LinkDown, address gone");

		r.joinUp();
		r.esp->connect(0, IpAddress(1, 2, 3, 4), 80, 0);
		r.run(20);
		r.host.events.clear();
		r.host.dev.clear();
		r.sim.reboot();
		r.run(20);
		check(r.host.has(0, SocketEvent::Failed) && r.host.hasDev(DeviceEvent::Failed), "module rebooted by itself: Failed");
		r.run(1200);
		check(r.esp->ready() && r.esp->linkUp() && r.sim.joined(), "re-initialised, and joined again by itself");

		r.sim.refuseConnect = true;
		r.esp->connect(1, IpAddress(1, 2, 3, 4), 81, 0);
		r.run(20);
		check(r.host.has(1, SocketEvent::Failed), "refused connect: Failed");
		check(r.host.wakes > 0, "the UART ISR asked for the driver thread to be woken");
		const espat_stats_t st = r.esp->stats();
		check(st.failures >= 2 && st.rxOverflows == 0, "stats(): failures counted, no overflows");
	}

	std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "PASSED", g_failures, g_failures == 1 ? "" : "s");
	return g_failures ? 1 : 0;
}
