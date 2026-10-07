/*
 * W5500_test.cpp
 *
 * Host test for w5500<TTransport>, the state machine on its own: one
 * thread, a simulated clock, a simulated chip (test/sim/SimW5500.h)
 * and a plain host standing in for xEthernet. No hardware, HAL or RTOS.
 *
 *   g++ -std=c++17 -Wall -Wextra -I../inc -I../../inc -I../../test/sim \
 *       -I<iTransport>/itransport/inc W5500_test.cpp -o W5500_test
 */

#include <cstdio>
#include <deque>
#include <memory>
#include <vector>
#include "W5500.h"
#include "SimW5500.h"
#include "SimDhcpServer.h"
#include "W5500Probe.h"

using namespace W5500;

static int g_failures = 0;
static void check(bool ok, const char *what) {
	std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok) ++g_failures;
}

// Stands in for xEthernet: plain queues, no threads.
struct Host : iNetDeviceHost {
	std::deque<uint8_t> rx[8], tx[8];
	size_t rxCap = 4096;
	std::vector<std::pair<int, SocketEvent>> events;
	std::vector<DeviceEvent> dev;

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
	void wakeFromIsr() override {}
	std::vector<NetConfig> addrs;

	bool overrun = false;
	bool has(int s, SocketEvent e) const {
		for (auto &p : events) if (p.first == s && p.second == e) return true;
		return false;
	}
	bool hasDev(DeviceEvent e) const {
		for (auto d : dev) if (d == e) return true;
		return false;
	}
	std::vector<uint8_t> takeRx(int s) { std::vector<uint8_t> v(rx[s].begin(), rx[s].end()); rx[s].clear(); return v; }
};

typedef w5500<FakeW5500Spi> Chip;

// Everything one test needs, on the heap: the simulated chip is large.
struct Rig {
	SimW5500 sim;
	Host host;
	std::unique_ptr<Chip> chip;
	uint32_t now = 1000;
	NetConfig cfg;

	explicit Rig(const w5500_param_t &p = w5500_param_t()) {
		chip.reset(new Chip(p, sim));
		chip->attach(host);
		cfg.mac = MacAddress(0x02, 0x08, 0xDC, 0x01, 0x02, 0x03);
		cfg.ip = IpAddress(192, 168, 1, 50);
		cfg.subnet = IpAddress(255, 255, 255, 0);
		cfg.gateway = IpAddress(192, 168, 1, 1);
	}
	// Run for ms of simulated time, polling every ms as the driver
	// thread would at worst.
	void run(uint32_t ms) {
		for (uint32_t i = 0; i < ms; ++i, ++now) {
			for (int j = 0; j < 20 && chip->poll(now) == 0; ++j) {}
		}
	}
	void start() { chip->configure(cfg); run(20); }

	// A DHCP server on the simulated network.
	SimDhcpServer dhcpSrv;
	int udpSends = 0;
	IpAddress lastUdpDst;
	uint16_t lastUdpPort = 0;
	void serveDhcp() {
		cfg.dhcp = true;
		sim.onUdpSend = [this](uint8_t s, IpAddress dst, uint16_t port, std::vector<uint8_t> data) {
			++udpSends;
			lastUdpDst = dst;
			lastUdpPort = port;
			const std::vector<uint8_t> reply = dhcpSrv.handle(data.data(), data.size());
			if (!reply.empty()) sim.peerSendUdp(s, dhcpSrv.server, 67, reply.data(), reply.size());
		};
	}
};

static std::vector<uint8_t> pattern(size_t n, uint8_t seed) {
	std::vector<uint8_t> v(n);
	for (size_t i = 0; i < n; ++i) v[i] = static_cast<uint8_t>(seed + i * 7 + (i >> 8));
	return v;
}

int main() {
	std::printf("start up\n");
	{
		w5500_param_t p;
		p.retryTime100us = 1000;
		p.retryCount = 5;
		Rig r(p);
		r.run(50);
		check(r.chip->state() == w5500_state_t::unconfigured && r.sim.transfers == 0, "does nothing before configure()");
		r.start();
		check(r.chip->ready() && r.host.hasDev(DeviceEvent::Ready), "Ready");
		check(r.host.hasDev(DeviceEvent::LinkUp) && r.chip->linkUp() && r.chip->speedMbps() == 100 && r.chip->fullDuplex(),
		      "LinkUp, 100 Mbps full duplex from PHYCFGR");
		check(r.sim.resets == 1, "soft reset first");
		check(r.chip->stats().inits == 1 && r.chip->stats().failures == 0 && r.chip->stats().transfers > 10, "stats()");
		bool mac = true, ip = true;
		for (int i = 0; i < 6; ++i) mac &= r.sim.common(w5500_SHAR + i) == r.cfg.mac.b[i];
		for (int i = 0; i < 4; ++i) ip &= r.sim.common(w5500_SIPR + i) == r.cfg.ip.b[i]
		                              && r.sim.common(w5500_GAR + i) == r.cfg.gateway.b[i]
		                              && r.sim.common(w5500_SUBR + i) == r.cfg.subnet.b[i];
		check(mac && ip, "SHAR, SIPR, GAR, SUBR written");
		check(r.sim.common(w5500_RTR) == 0x03 && r.sim.common(w5500_RTR + 1) == 0xE8 && r.sim.common(w5500_RCR) == 5, "RTR, RCR from param");
		check(r.sim.common(w5500_SIMR) == 0xFF, "socket interrupts enabled");

		r.sim.link = false;
		r.run(600);
		check(r.host.dev.back() == DeviceEvent::LinkDown && !r.chip->linkUp() && r.chip->speedMbps() == 0, "link loss noticed by the PHY poll");
	}

	std::printf("bring-up probe\n");
	{
		SimW5500 *sim = new SimW5500;
		FakeW5500Spi spi(*sim);
		static uint32_t clock = 0;
		w5500_probe probe(spi, [] { return clock++; });
		uint8_t v = 0, phy = 0;
		static uint8_t scratch[2048];
		check(probe.version(v) == w5500_probe::Result::Ok && v == 0x04, "VERSIONR 0x04");
		check(probe.writeRead() == w5500_probe::Result::Ok, "SHAR write/read-back");
		check(probe.bufferTest(scratch, sizeof scratch) == w5500_probe::Result::Ok, "2 KB buffer write/read-back");
		check(probe.phy(phy) == w5500_probe::Result::Ok && (phy & 1), "PHY link");
		sim->stuckRead = 0x00;
		check(probe.version(v) == w5500_probe::Result::ReadsZero, "MISO stuck low: ReadsZero");
		sim->stuckRead = 0xFF;
		check(probe.version(v) == w5500_probe::Result::ReadsOnes, "MISO floating: ReadsOnes");
		sim->stuckRead = -1;
		sim->version = 0x02;
		check(probe.version(v) == w5500_probe::Result::WrongVersion, "another chip: WrongVersion");
		sim->version = 0x04;
		sim->dropWrites = true;
		check(probe.writeRead() == w5500_probe::Result::NoWrite, "MOSI open: NoWrite");
		sim->dropWrites = false;
		sim->spiRefuse = true;
		check(probe.version(v) == w5500_probe::Result::BusError, "bus never takes the transfer: BusError, after the timeout");
		check(std::strlen(w5500_probe::describe(w5500_probe::Result::ReadsOnes)) > 20, "each result explained");
		delete sim;
	}

	std::printf("static address reported\n");
	{
		Rig r;
		r.start();
		check(r.host.addrs.size() == 1 && r.host.addrs[0].ip == r.cfg.ip, "addressChanged() with the static address at start-up");
		check(r.chip->socketCount() == 7, "7 sockets for the interface: socket 7 kept for DHCP");
		w5500_param_t p;
		p.dhcp = false;
		Rig r8(p);
		check(r8.chip->socketCount() == 8, "all 8 with dhcp off");
		r8.serveDhcp();
		r8.start();
		check(!r8.chip->ready() && r8.host.hasDev(DeviceEvent::Failed), "NetConfig::dhcp without the socket kept for it: Failed");
	}

	std::printf("DHCP\n");
	{
		Rig r;
		r.serveDhcp();
		r.dhcpSrv.silent = true;	// server slow to answer
		r.start();
		check(r.chip->ready() && r.host.addrs.empty(), "chip ready, no address yet");
		check(r.sim.status(7) == w5500_SOCK_UDP && r.sim.sockReg16(7, w5500_Sn_PORT) == 68, "socket 7 open, UDP, port 68");
		check(r.udpSends == 1 && r.lastUdpDst == IpAddress(255, 255, 255, 255) && r.lastUdpPort == 67, "DISCOVER broadcast to port 67");
		check(r.sim.common(w5500_SIPR) == 0 && r.sim.common(w5500_SIPR + 3) == 0, "SIPR 0.0.0.0 meanwhile");

		r.chip->connect(0, IpAddress(192, 168, 1, 10), 80, 0);
		r.run(50);
		check(!r.host.has(0, SocketEvent::Connected) && r.sim.status(0) == w5500_SOCK_CLOSED, "connect() held until there is an address");

		r.dhcpSrv.silent = false;
		r.run(2100);	// the DISCOVER retry
		check(r.host.addrs.size() == 1 && r.host.addrs[0].ip == r.dhcpSrv.offerIp, "address reported once leased");
		const NetConfig &a = r.host.addrs[0];
		check(a.gateway == r.dhcpSrv.router && a.subnet == r.dhcpSrv.subnet && a.dns == r.dhcpSrv.dns && a.dhcp,
		      "with gateway, mask and DNS");
		bool sipr = true, gar = true;
		for (int i = 0; i < 4; ++i) {
			sipr &= r.sim.common(w5500_SIPR + i) == r.dhcpSrv.offerIp.b[i];
			gar &= r.sim.common(w5500_GAR + i) == r.dhcpSrv.router.b[i];
		}
		check(sipr && gar, "written to SIPR/GAR");
		check(r.host.has(0, SocketEvent::Connected), "the held connect() then goes ahead");

		// Renewal at T1 (half of 3600 s), same address: nothing changes.
		r.dhcpSrv.leaseSec = 3600;
		const int requests = r.dhcpSrv.requests;
		for (int i = 0; i < 1810; ++i) r.run(1000);
		check(r.dhcpSrv.requests == requests + 1 && r.dhcpSrv.lastCiaddr == r.dhcpSrv.offerIp, "renewed at T1");
		check(r.host.addrs.size() == 1 && !r.host.has(0, SocketEvent::Failed), "same address: no change, connection kept");

		// Cable out and back: the lease is checked at once, and refused here.
		r.dhcpSrv.nak = true;
		r.sim.link = false;
		r.run(600);
		r.sim.link = true;
		r.run(600);
		check(r.host.addrs.size() >= 2 && r.host.addrs[1].ip.isZero(), "link back, lease NAKed: address lost");
		check(r.host.has(0, SocketEvent::Failed) && r.sim.status(0) == w5500_SOCK_CLOSED, "its connection Failed, and closed on the chip");
		check(r.sim.common(w5500_SIPR) == 0, "SIPR cleared");
		r.dhcpSrv.nak = false;
		r.dhcpSrv.offerIp = IpAddress(192, 168, 1, 88);
		r.run(5000);
		check(r.host.addrs.back().ip == IpAddress(192, 168, 1, 88), "and a new lease is taken");
	}

	std::printf("not a W5500\n");
	{
		Rig r;
		r.sim.version = 0x00; // nothing on the bus
		r.start();
		check(!r.chip->ready() && r.host.hasDev(DeviceEvent::Failed), "Failed, not Ready");
		r.run(2100);
		check(r.sim.resets >= 3, "keeps trying, after a back-off");
		r.sim.version = 0x04;
		r.run(1100);
		check(r.chip->ready() && r.host.dev.back() == DeviceEvent::LinkUp, "comes up once the chip answers");
	}

	std::printf("bad buffer sizes\n");
	{
		w5500_param_t p;
		for (int i = 0; i < 8; ++i) p.rxBufKb[i] = 4; // 32 KB, more than the chip has
		Rig r(p);
		r.start();
		check(!r.chip->ready() && r.host.hasDev(DeviceEvent::Failed) && r.sim.transfers == 0, "refused before touching the chip");
	}

	std::printf("connect\n");
	{
		Rig r;
		r.start();
		check(r.chip->connect(0, IpAddress(192, 168, 1, 10), 8080, 0), "connect() accepted");
		r.run(10);
		check(r.host.has(0, SocketEvent::Connected), "Connected");
		check(r.sim.connectedTo(0) == IpAddress(192, 168, 1, 10) && r.sim.connectedPort(0) == 8080, "to the right address and port");
		check(r.sim.sockReg16(0, w5500_Sn_PORT) == 49152, "ephemeral local port");
		check(r.sim.sockReg(0, w5500_Sn_MR) == (w5500_Sn_MR_TCP | w5500_Sn_MR_ND), "TCP, no delayed ACK");

		r.chip->connect(1, IpAddress(10, 0, 0, 1), 80, 5000);
		r.run(10);
		check(r.host.has(1, SocketEvent::Connected) && r.sim.sockReg16(1, w5500_Sn_PORT) == 5000, "second socket, local port as asked");
	}

	std::printf("connect refused / timed out\n");
	{
		Rig r;
		r.start();
		r.sim.connectPolicy = SimW5500::Connect::Refuse;
		r.chip->connect(2, IpAddress(192, 168, 1, 99), 1234, 0);
		r.run(10);
		check(r.host.has(2, SocketEvent::Failed) && !r.host.has(2, SocketEvent::Connected), "Failed");
		check(r.sim.status(2) == w5500_SOCK_CLOSED, "socket closed on the chip");
	}

	std::printf("receive: wraps, and waits for room\n");
	{
		Rig r;
		r.host.rxCap = 700; // smaller than one chunk: the driver has to stop and wait
		r.start();
		r.chip->connect(0, IpAddress(192, 168, 1, 10), 80, 0);
		r.run(10);
		const std::vector<uint8_t> data = pattern(5000, 3); // > 2 KB buffer, and Sn_RX_RD starts at 0xFFF0
		std::vector<uint8_t> got;
		size_t sent = 0;
		for (int i = 0; i < 200 && got.size() < data.size(); ++i) {
			sent += r.sim.peerSend(0, data.data() + sent, data.size() - sent);
			r.run(6);
			const auto chunk = r.host.takeRx(0);
			got.insert(got.end(), chunk.begin(), chunk.end());
		}
		check(got == data, "5000 bytes arrive intact across the 16-bit pointer wrap and the 2 KB buffer wrap");
		check(!r.host.overrun, "never more than rxSpace() at a time");

		r.host.rxCap = 0; // host full
		r.sim.peerSend(0, data.data(), 100);
		r.run(20);
		check(r.host.rx[0].empty(), "nothing delivered while the host has no room");
		r.host.rxCap = 4096;
		r.run(10); // the driver thread would be kicked; here the next pass sees the room
		check(r.host.rx[0].size() == 100, "delivered once there is room");
	}

	std::printf("send\n");
	{
		Rig r;
		r.start();
		r.chip->connect(0, IpAddress(192, 168, 1, 10), 80, 0);
		r.run(10);
		const std::vector<uint8_t> data = pattern(5000, 9);
		r.host.tx[0].assign(data.begin(), data.end());
		r.run(50);
		check(r.sim.sent(0) == data, "5000 bytes sent intact, across the pointer and buffer wraps");
		check(r.sim.sends(0) >= 3, "in several SENDs: the chip's TX buffer is 2 KB");

		r.sim.deferSendOk = true;
		r.host.tx[0].assign(10, 0x55);
		r.run(5);
		const int sends = r.sim.sends(0);
		r.host.tx[0].assign(10, 0x66);
		r.run(20);
		check(r.sim.sends(0) == sends, "no second SEND before SEND_OK");
		r.sim.sendOk(0);
		r.run(10);
		check(r.sim.sends(0) == sends + 1 && r.host.tx[0].empty(), "next SEND once SEND_OK arrives");
	}

	std::printf("peer closes, with data still to read\n");
	{
		Rig r;
		r.host.rxCap = 50;
		r.start();
		r.chip->connect(3, IpAddress(192, 168, 1, 10), 80, 0);
		r.run(10);
		const std::vector<uint8_t> data = pattern(120, 1);
		r.sim.peerSend(3, data.data(), data.size());
		r.sim.peerClose(3);
		r.run(10);
		check(!r.host.has(3, SocketEvent::Closed), "not Closed while data is still on the chip");
		std::vector<uint8_t> got;
		for (int i = 0; i < 10; ++i) {
			auto c = r.host.takeRx(3);
			got.insert(got.end(), c.begin(), c.end());
			r.run(10);
		}
		check(got == data, "all of it delivered");
		check(r.host.has(3, SocketEvent::Closed) && r.sim.status(3) == w5500_SOCK_CLOSED, "then our side closed, and Closed");
	}

	std::printf("we close\n");
	{
		Rig r;
		r.start();
		r.chip->connect(0, IpAddress(192, 168, 1, 10), 80, 0);
		r.run(10);
		r.chip->close(0);
		r.run(10);
		check(r.host.has(0, SocketEvent::Closed) && r.sim.status(0) == w5500_SOCK_CLOSED, "graceful close: Closed");

		r.host.events.clear();
		r.chip->close(0);
		r.run(5);
		check(r.host.has(0, SocketEvent::Closed), "closing a closed socket still answers Closed");

		r.sim.closeCompletes = false; // peer never finishes the close
		r.host.events.clear();
		r.chip->connect(0, IpAddress(192, 168, 1, 10), 80, 0);
		r.run(10);
		r.chip->close(0);
		r.run(500);
		check(!r.host.has(0, SocketEvent::Closed), "waits for the peer...");
		r.run(1600);
		check(r.host.has(0, SocketEvent::Closed) && r.sim.status(0) == w5500_SOCK_CLOSED, "...until the close timeout, then forces CLOSE");
	}

	std::printf("listen\n");
	{
		Rig r;
		r.start();
		r.chip->listen(1, 8080);
		r.run(10);
		check(r.sim.status(1) == w5500_SOCK_LISTEN && r.sim.sockReg16(1, w5500_Sn_PORT) == 8080, "listening on 8080");
		check(r.host.events.size() == 1 && r.host.has(1, SocketEvent::Listening), "Listening reported, nothing else yet");
		r.sim.peerConnect(1);
		r.run(10);
		check(r.host.has(1, SocketEvent::Connected), "Connected when a peer arrives");
	}

	std::printf("interrupt line\n");
	{
		w5500_param_t p;
		p.pollMs = 1000; // as with INT wired: polling is only a backstop
		Rig r(p);
		r.start();
		r.chip->connect(0, IpAddress(192, 168, 1, 10), 80, 0);
		r.run(10);
		r.run(1000); // let the first backstop poll go by
		const uint8_t hi[2] = {'h', 'i'};
		r.sim.peerSend(0, hi, 2);
		r.run(100);
		check(r.host.rx[0].empty(), "without the interrupt, nothing for a while");
		r.chip->interrupt();
		r.run(2);
		check(r.host.rx[0].size() == 2, "interrupt() gets it read at once");
	}

	std::printf("bus failure\n");
	{
		Rig r;
		r.start();
		r.chip->connect(0, IpAddress(192, 168, 1, 10), 80, 0);
		r.chip->connect(1, IpAddress(192, 168, 1, 11), 80, 0);
		r.run(10);
		r.host.events.clear();
		r.host.dev.clear();
		r.sim.spiFailNext = true;
		r.run(10);
		check(r.host.has(0, SocketEvent::Failed) && r.host.has(1, SocketEvent::Failed), "every open socket Failed");
		check(r.host.hasDev(DeviceEvent::Failed) && !r.chip->ready(), "device Failed");
		r.run(1100);
		check(r.chip->ready() && r.host.hasDev(DeviceEvent::Ready) && r.sim.resets == 2, "re-initialised after the back-off");
	}

	std::printf("close before the chip is up\n");
	{
		Rig r;
		r.chip->close(4);
		check(r.host.has(4, SocketEvent::Closed), "answered at once");
		check(!r.chip->connect(4, IpAddress(1, 2, 3, 4), 80, 0), "connect() refused before configure()");
	}

	std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "PASSED", g_failures, g_failures == 1 ? "" : "s");
	return g_failures ? 1 : 0;
}
