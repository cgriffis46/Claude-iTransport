/*
 * Winc1500_test.cpp
 *
 * Host test for winc1500<TTransport>, the state machine on its own: one
 * thread, a simulated clock, a simulated module (test/sim/SimWinc1500.h,
 * byte by byte on the SPI) and a plain host standing in for xWifi. No
 * hardware, HAL or RTOS.
 *
 *   g++ -std=c++17 -Wall -Wextra -I../inc -I../../inc -I../../test/sim \
 *       -I<iTransport>/itransport/inc Winc1500_test.cpp -o Winc1500_test
 */

#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <string>
#include <vector>
#include "Winc1500.h"
#include "SimWinc1500.h"

using namespace WINC1500;

static int g_failures = 0;
static void check(bool ok, const char *what) {
	std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok) ++g_failures;
}

// Stands in for xWifi: plain queues, no threads.
struct Host : iNetDeviceHost {
	std::deque<uint8_t> rx[8], tx[8];
	size_t rxCap[8] = {4096, 4096, 4096, 4096, 4096, 4096, 4096, 4096};
	std::vector<std::pair<int, SocketEvent>> events;
	std::vector<DeviceEvent> dev;
	std::vector<NetConfig> addrs;
	int resolvedCount = 0, timeCount = 0;
	bool resolvedOk = false, timeOk = false, overrun = false;
	IpAddress resolvedIp;
	uint64_t timeUnixMs = 0;
	uint32_t timeAtMs = 0;

	size_t rxSpace(uint8_t s) override { return rxCap[s] > rx[s].size() ? rxCap[s] - rx[s].size() : 0; }
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
	void resolved(bool ok, const IpAddress &ip) override { ++resolvedCount; resolvedOk = ok; resolvedIp = ip; }
	void timeReceived(bool ok, uint64_t unixMs, uint32_t atMs) override {
		++timeCount; timeOk = ok; timeUnixMs = unixMs; timeAtMs = atMs;
	}
	void wakeFromIsr() override {}

	int count(int s, SocketEvent e) const {
		int n = 0;
		for (auto &p : events) if (p.first == s && p.second == e) ++n;
		return n;
	}
	bool has(int s, SocketEvent e) const { return count(s, e) > 0; }
	int countDev(DeviceEvent e) const {
		int n = 0;
		for (auto d : dev) if (d == e) ++n;
		return n;
	}
	bool hasDev(DeviceEvent e) const { return countDev(e) > 0; }
	std::vector<uint8_t> takeRx(int s) { std::vector<uint8_t> v(rx[s].begin(), rx[s].end()); rx[s].clear(); return v; }
	void send(int s, const std::vector<uint8_t> &v) { tx[s].insert(tx[s].end(), v.begin(), v.end()); }
};

typedef winc1500<FakeWincSpi> Winc;

static SimWinc1500 *g_resetSim = nullptr;
static int g_resetPulses = 0;
static void resetPin(bool asserted) {
	if (asserted) ++g_resetPulses;
	if (g_resetSim) g_resetSim->resetPin(asserted);
}

// Everything one test needs.
struct Rig {
	SimWinc1500 sim;
	FakeWincSpi *spi = nullptr;
	Host host;
	std::unique_ptr<Winc> chip;
	uint32_t now = 1000;
	NetConfig cfg;
	bool irqWired = false;

	explicit Rig(winc_param_t p = winc_param_t(), bool withResetPin = true) {
		g_resetSim = &sim;
		if (withResetPin) p.hardReset = resetPin;
		chip.reset(new Winc(p, sim));
		chip->attach(host);
		cfg.dhcp = true;
		sim.aps.push_back({"home", "correct horse"});
		sim.aps.push_back({"cafe", ""});
	}
	~Rig() { g_resetSim = nullptr; }
	void wireIrq() {
		irqWired = true;
		sim.onIrq = [this]() { chip->interrupt(); };
	}
	// Run for ms of simulated time: the module's clock, then the driver
	// called as its thread would be, until it asks to wait.
	void run(uint32_t ms) {
		for (uint32_t i = 0; i < ms; ++i, ++now) {
			sim.tick(now);
			for (int j = 0; j < 400 && chip->poll(now) == 0; ++j) {}
		}
	}
	// Until cond, at most ms.
	template <typename F>
	bool runUntil(uint32_t ms, F cond) {
		for (uint32_t i = 0; i < ms; ++i) {
			if (cond()) return true;
			run(1);
		}
		return cond();
	}
	bool start() {
		chip->configure(cfg);
		return runUntil(2000, [&] { return host.hasDev(DeviceEvent::Ready); });
	}
	bool up(const char *ssid = "home", const char *pass = "correct horse") {
		if (!start()) return false;
		chip->join(ssid, pass);
		return runUntil(3000, [&] { return host.hasDev(DeviceEvent::LinkUp); });
	}
	// Run, reading socket s as a reader would, until want bytes or ms.
	std::vector<uint8_t> drain(int s, size_t want, uint32_t ms) {
		std::vector<uint8_t> got;
		for (uint32_t i = 0; i < ms && got.size() < want; ++i) {
			run(1);
			auto v = host.takeRx(s);
			got.insert(got.end(), v.begin(), v.end());
		}
		return got;
	}
	void misuseReport() {
		for (auto &m : sim.misuseWhat) std::printf("    misuse: %s\n", m.c_str());
	}
};

static std::vector<uint8_t> pattern(size_t n, uint8_t seed) {
	std::vector<uint8_t> v(n);
	for (size_t i = 0; i < n; ++i) v[i] = static_cast<uint8_t>(seed + i * 7 + (i >> 8));
	return v;
}

static void testCrc7() {
	std::printf("CRC-7\n");
	// Entries of nmspi.c's crc7_syndrome_table: table[d] is the CRC of
	// the one byte d from 0.
	const uint8_t table[] = {0x00, 0x09, 0x12, 0x1b, 0x24, 0x2d, 0x36, 0x3f, 0x48, 0x41, 0x5a, 0x53};
	bool ok = true;
	for (uint8_t d = 0; d < sizeof table; ++d) ok &= WINC::crc7(0, &d, 1) == table[d];
	uint8_t d = 0x80;
	ok &= WINC::crc7(0, &d, 1) == 0x41;   // table row 16
	d = 0xFF;
	ok &= WINC::crc7(0, &d, 1) == 0x79;   // the table's last entry
	check(ok, "crc7() matches Microchip's table");
}

static void testStartup() {
	std::printf("start-up\n");
	{
		Rig r;
		g_resetPulses = 0;
		check(r.start(), "Ready, after a hardware reset");
		check(g_resetPulses == 1, "the reset pin pulsed once");
		check(!r.sim.crcOn(), "the SPI CRC turned off");
		check((r.sim.reg(0xE824) & 0x70) == 0x50, "8 KB data packets");
		check(r.sim.hostVersion == 0x13521352, "the host reported itself as 19.5.2");
		check(r.sim.gp1 == 0x102, "start-up bits: reserved1, and the PMU (rev 3A0)");
		check(r.sim.irqEnabled(), "IRQN enabled");
		const winc_version_t v = r.chip->firmware();
		check(v.major == 19 && v.minor == 6 && v.patch == 1, "firmware 19.6.1 read from tstrM2mRev");
		check(r.chip->chipId() == 0x1003A0, "chip ID");
		check(r.chip->mac().b[0] == 0xF8 && r.chip->mac().b[5] == 0x03, "the module's MAC");
		check(r.sim.dhcpOn && r.sim.staticIp.empty(), "DHCP on (asked for)");
		check(r.chip->stats().inits == 1 && r.chip->stats().failures == 0, "one start, no failures");
		check(r.sim.misuse == 0, "no misuse");
		r.misuseReport();
	}
	{
		winc_param_t p;
		Rig r(p, false);
		const int resetsBefore = r.sim.resets;
		check(r.start() && r.sim.resets == resetsBefore + 1, "without a reset pin: a global reset by register, then Ready");
		check(r.sim.misuse == 0, "no misuse");
	}
	{
		winc_param_t p;
		Rig r(p, false);
		r.sim.crcOffAtStart = true;
		r.sim.respLatency = 0;   // the echo comes while the CRC byte the module isn't expecting goes out
		r.sim.resetPin(true);
		r.sim.resetPin(false);   // the module comes up with the CRC already off
		check(r.start() && r.chip->stats().spiRetries > 0, "the CRC left off from before: tried with it on, then off");
		check(r.sim.misuse == 0, "no misuse");
	}
	{
		bool all = true;
		for (int lat = 0; lat <= 5; ++lat) {
			Rig r;
			r.sim.respLatency = lat;
			all &= r.start() && r.sim.misuse == 0 && r.chip->stats().spiRetries == 0;
		}
		check(all, "0 to 5 idle bytes before each answer: polled through, no retries");
	}
	{
		Rig r;
		r.sim.respLatency = 12;   // more than the 10 polls allowed
		r.chip->configure(r.cfg);
		r.run(3000);
		check(!r.host.hasDev(DeviceEvent::Ready) && r.host.hasDev(DeviceEvent::Failed), "answers too late: Failed");
	}
	{
		Rig r;
		r.sim.stuckMiso = 0xFF;
		r.chip->configure(r.cfg);
		r.run(500);
		check(r.host.hasDev(DeviceEvent::Failed) && r.chip->error() == winc_error_t::no_chip, "MISO high (no module): no_chip");
		check(r.chip->stats().spiRetries >= 9, "after retries with SPI resets");
		const uint32_t f = r.chip->stats().failures;
		r.run(1500);
		check(r.chip->stats().failures > f, "and it tries again by itself");
		r.sim.stuckMiso = -1;
		check(r.runUntil(5000, [&] { return r.host.hasDev(DeviceEvent::Ready); }), "the module answering again: Ready");
	}
	{
		Rig r;
		r.sim.chipIdRaw = 0x1000F0;
		r.sim.resetPin(true); r.sim.resetPin(false);
		r.chip->configure(r.cfg);
		r.run(500);
		check(r.chip->error() == winc_error_t::wrong_chip, "another chip ID: wrong_chip");
	}
	{
		Rig r;
		r.sim.fwMinor = 4; r.sim.fwPatch = 4;
		r.chip->configure(r.cfg);
		r.run(500);
		check(r.chip->error() == winc_error_t::firmware_too_old && !r.host.hasDev(DeviceEvent::Ready), "firmware 19.4.4: firmware_too_old");
	}
	{
		Rig r;
		r.sim.oldFirmware = true;
		r.chip->configure(r.cfg);
		r.run(500);
		check(r.chip->error() == winc_error_t::firmware_too_old, "firmware 19.3 (no revision block): firmware_too_old");
	}
	{
		Rig r;
		r.sim.needMinor = 7;
		r.chip->configure(r.cfg);
		r.run(500);
		check(r.chip->error() == winc_error_t::firmware_too_new, "firmware needing a 19.7 host driver: firmware_too_new");
	}
	{
		Rig r;
		r.sim.firmwareHangs = true;
		r.chip->configure(r.cfg);
		r.run(5000);
		check(r.chip->error() == winc_error_t::firmware_timeout, "firmware never up: firmware_timeout");
	}
	{
		Rig r;
		r.sim.spiRefuse = true;
		r.chip->configure(r.cfg);
		r.run(4000);
		check(r.host.hasDev(DeviceEvent::Failed) && !r.host.hasDev(DeviceEvent::Ready), "the bus refusing every transfer: Failed");
	}
}


static void testWifi() {
	std::printf("Wi-Fi\n");
	{
		Rig r;
		check(r.up(), "join a WPA2 network: LinkUp");
		check(r.sim.lastSsid == "home" && r.sim.lastPass == "correct horse" && r.sim.lastSecType == 2 && r.sim.lastNoSave == 1,
		      "tstrM2mWifiConnect: SSID, passphrase, WPA-PSK, not saved to the module's flash");
		check(!r.host.addrs.empty() && r.host.addrs.back().ip == IpAddress(192, 168, 4, 23) &&
		          r.host.addrs.back().gateway == IpAddress(192, 168, 4, 1) && r.host.addrs.back().subnet == IpAddress(255, 255, 255, 0) &&
		          r.host.addrs.back().dhcp,
		      "the DHCP address reported, with gateway and subnet");
		r.run(50);
		check(r.chip->rssi() == -55, "RSSI asked for once up");
		r.sim.rssi = -70;
		r.run(10100);
		check(r.chip->rssi() == -70, "and again every rssiPollMs");
		r.sim.dhcpRenew(IpAddress(192, 168, 4, 99));
		r.run(20);
		check(r.host.addrs.back().ip == IpAddress(192, 168, 4, 99) && r.host.countDev(DeviceEvent::LinkUp) >= 1, "a new DHCP address reported");
		r.chip->leave();
		check(r.runUntil(500, [&] { return r.host.hasDev(DeviceEvent::LinkDown); }), "leave(): LinkDown");
		check(r.host.addrs.back().ip.isZero() && r.chip->rssi() == 0, "the address gone, RSSI 0");
		check(r.sim.misuse == 0, "no misuse");
		r.misuseReport();
	}
	{
		Rig r;
		r.start();
		r.chip->join("cafe", "");
		check(r.runUntil(2000, [&] { return r.host.hasDev(DeviceEvent::LinkUp); }) && r.sim.lastSecType == 1, "an open network");
	}
	{
		Rig r;
		r.start();
		r.chip->join("home", "wrong horse");
		check(r.runUntil(2000, [&] { return r.host.hasDev(DeviceEvent::JoinFailed); }) && r.chip->joinError() == WINC::err_auth_fail,
		      "a wrong passphrase: JoinFailed, auth_fail");
		r.chip->join("nowhere", "whatever123");
		r.host.dev.clear();
		check(r.runUntil(2000, [&] { return r.host.hasDev(DeviceEvent::JoinFailed); }) && r.chip->joinError() == WINC::err_scan_fail,
		      "no such network: JoinFailed, scan_fail");
		const int joins = r.sim.joins;
		r.host.dev.clear();
		r.chip->join("home", "short");
		r.run(50);
		check(r.host.hasDev(DeviceEvent::JoinFailed) && r.sim.joins == joins, "a 5-character passphrase: JoinFailed, nothing sent");
		r.host.dev.clear();
		r.chip->join("home", "correct horse");
		check(r.runUntil(2000, [&] { return r.host.hasDev(DeviceEvent::LinkUp); }), "then the right one: LinkUp");
		r.host.dev.clear();
		r.chip->join("cafe", "");
		check(r.runUntil(3000, [&] { return r.host.hasDev(DeviceEvent::LinkUp); }) && r.sim.lastSsid == "cafe" &&
		          r.host.hasDev(DeviceEvent::LinkDown),
		      "join() while joined: leaves (LinkDown), joins the other (LinkUp)");
		check(r.sim.misuse == 0, "no misuse");
	}
	{
		Rig r;
		r.cfg.dhcp = false;
		r.cfg.ip = IpAddress(10, 0, 0, 5);
		r.cfg.subnet = IpAddress(255, 255, 255, 0);
		r.cfg.gateway = IpAddress(10, 0, 0, 1);
		r.cfg.dns = IpAddress(10, 0, 0, 2);
		check(r.up(), "a static address: LinkUp on association");
		const std::vector<uint8_t> want = {10, 0, 0, 5, 10, 0, 0, 1, 10, 0, 0, 2, 255, 255, 255, 0, 0, 0, 0, 0};
		check(!r.sim.dhcpOn && r.sim.staticIp == want, "DHCP off, tstrM2MIPConfig: address, gateway, DNS, subnet");
		check(r.host.addrs.back().ip == IpAddress(10, 0, 0, 5) && !r.host.addrs.back().dhcp, "the static address reported");
	}
	{
		Rig r;
		check(r.up(), "up");
		r.host.dev.clear();
		r.sim.apDrops();
		check(r.runUntil(100, [&] { return r.host.hasDev(DeviceEvent::LinkDown); }), "the access point gone: LinkDown");
		check(r.runUntil(7000, [&] { return r.host.hasDev(DeviceEvent::LinkUp); }) && r.sim.joins == 2,
		      "joined again by itself, after rejoin_ms");
		r.host.dev.clear();
		r.sim.aps.clear();
		r.sim.apDrops();
		r.run(20000);
		check(r.host.hasDev(DeviceEvent::LinkDown) && !r.host.hasDev(DeviceEvent::JoinFailed) && r.sim.joins >= 5,
		      "while it stays away: tries every 5 s, no JoinFailed");
		r.sim.aps.push_back({"home", "correct horse"});
		check(r.runUntil(7000, [&] { return r.host.hasDev(DeviceEvent::LinkUp); }), "back: LinkUp");
		r.chip->leave();
		r.run(100);
		const int joins = r.sim.joins;
		r.sim.apDrops();
		r.run(10000);
		check(r.sim.joins == joins, "after leave(): no more tries");
		check(r.sim.misuse == 0, "no misuse");
	}
}

static void testTcp() {
	std::printf("TCP client\n");
	{
		Rig r;
		r.sim.servers["192.168.4.10:1883"] = SimWinc1500::Policy::Accept;
		r.sim.servers["192.168.4.10:80"] = SimWinc1500::Policy::Refuse;
		r.sim.servers["192.168.4.10:8080"] = SimWinc1500::Policy::Silent;
		check(!r.chip->connect(0, IpAddress(192, 168, 4, 10), 1883, 0), "connect() before configure(): false");
		check(r.start(), "started");
		check(r.chip->connect(0, IpAddress(192, 168, 4, 10), 1883, 0), "connect() before an address: taken, held");
		r.run(100);
		check(r.sim.connCount() == 0 && !r.host.has(0, SocketEvent::Connected), "nothing sent while not joined");
		r.chip->join("home", "correct horse");
		check(r.runUntil(3000, [&] { return r.host.hasDev(DeviceEvent::LinkUp); }), "joined");
		check(r.runUntil(200, [&] { return r.host.has(0, SocketEvent::Connected); }), "then Connected");
		const std::vector<uint8_t> out = pattern(10000, 3);
		r.host.send(0, out);
		r.runUntil(2000, [&] { return r.sim.fromDevice(0).size() == out.size(); });
		r.run(20);
		check(r.sim.fromDevice(0) == out, "10 KB sent, whole, in order (SENDs of 1400 at most)");
		check(r.chip->stats().txBytes == out.size(), "txBytes, as each SEND is answered");
		const std::vector<uint8_t> in = pattern(20000, 9);
		r.sim.peerSend(0, in.data(), in.size());
		check(r.drain(0, in.size(), 3000) == in && !r.host.overrun, "20 KB received, whole, never more than rxSpace()");
		r.sim.peerSend(0, std::string("bye"));
		r.sim.peerClose(0);
		check(r.runUntil(200, [&] { return r.host.has(0, SocketEvent::Closed); }), "the peer's close: Closed");
		check(r.host.takeRx(0) == std::vector<uint8_t>({'b', 'y', 'e'}), "after its last bytes");
		r.run(20);
		check(r.sim.conn(0).deviceClosed && r.sim.openSockets() == 0, "and the module's socket closed");

		check(r.chip->connect(1, IpAddress(192, 168, 4, 10), 80, 0), "connect() to a port that refuses");
		check(r.runUntil(200, [&] { return r.host.has(1, SocketEvent::Failed); }), "Failed");
		check(r.chip->connect(2, IpAddress(192, 168, 4, 10), 8080, 0), "connect() that's never answered");
		r.run(29000);
		check(!r.host.has(2, SocketEvent::Failed), "still waiting at 29 s");
		check(r.runUntil(2000, [&] { return r.host.has(2, SocketEvent::Failed); }), "Failed after 30 s");
		r.run(20);
		check(r.sim.openSockets() == 0, "its module socket closed");
		check(!r.chip->connect(7, IpAddress(1, 2, 3, 4), 1, 0) && !r.chip->connect(0, IpAddress(1, 2, 3, 4), 0, 0),
		      "connect(): socket out of range, or port 0: false");
		check(r.sim.misuse == 0, "no misuse");
		r.misuseReport();
	}
	std::printf("flow control\n");
	{
		Rig r;
		r.sim.servers["192.168.4.10:1"] = SimWinc1500::Policy::Accept;
		r.sim.servers["192.168.4.10:2"] = SimWinc1500::Policy::Accept;
		r.up();
		r.chip->connect(0, IpAddress(192, 168, 4, 10), 1, 0);
		r.chip->connect(1, IpAddress(192, 168, 4, 10), 2, 0);
		r.runUntil(300, [&] { return r.host.has(0, SocketEvent::Connected) && r.host.has(1, SocketEvent::Connected); });
		r.host.rxCap[0] = 300;   // a small buffer, read slowly
		const std::vector<uint8_t> a = pattern(6000, 1), b = pattern(6000, 2);
		r.sim.peerSend(0, a.data(), a.size());
		r.sim.peerSend(1, b.data(), b.size());
		std::vector<uint8_t> gotA, gotB;
		for (int i = 0; i < 1000 && (gotA.size() < a.size() || gotB.size() < b.size()); ++i) {
			r.run(5);
			if (i % 4 == 0) { auto v = r.host.takeRx(0); gotA.insert(gotA.end(), v.begin(), v.end()); }
			auto w = r.host.takeRx(1);
			gotB.insert(gotB.end(), w.begin(), w.end());
		}
		check(gotA == a && gotB == b && !r.host.overrun, "a reader 300 bytes every 20 ms and a fast one: both whole");
		check(r.chip->stats().heldReplies > 0, "replies waited in the module while the slow reader caught up");
		r.host.rxCap[0] = 0;   // a reader that stops
		r.sim.peerSend(0, a.data(), 100);
		r.run(100);
		r.sim.peerSend(1, b.data(), 500);
		r.run(100);
		check(r.host.rx[1].size() == 500, "a stopped reader with nothing buffered doesn't hold up the other socket");
		r.host.rxCap[0] = 4096;
		r.run(50);
		check(r.host.takeRx(0).size() == 100, "and gets its data once it reads");
		check(r.sim.misuse == 0, "no misuse");
	}
}

static void testListen() {
	std::printf("listening\n");
	Rig r;
	r.up();
	check(r.chip->listen(0, 80), "listen(0, 80)");
	check(r.runUntil(200, [&] { return r.host.has(0, SocketEvent::Listening); }), "Listening (BIND, then LISTEN)");
	const int c = r.sim.peerConnect(80);
	check(c >= 0 && r.runUntil(200, [&] { return r.host.has(0, SocketEvent::Connected); }), "a peer: Connected on socket 0");
	r.sim.peerSend(c, std::string("GET / HTTP/1.1\r\n\r\n"));
	r.runUntil(200, [&] { return r.host.rx[0].size() == 18; });
	check(r.host.rx[0].size() == 18, "its request");
	r.host.send(0, std::vector<uint8_t>({'O', 'K'}));
	r.runUntil(200, [&] { return r.sim.fromDevice(c).size() == 2; });
	check(r.sim.fromDevice(c) == std::vector<uint8_t>({'O', 'K'}), "the answer");
	check(r.chip->listen(1, 80), "listen(1, 80) while 0 is connected");
	check(r.runUntil(50, [&] { return r.host.has(1, SocketEvent::Listening); }), "Listening at once: the module's socket is still listening");
	const int c2 = r.sim.peerConnect(80);
	check(c2 >= 0 && r.runUntil(200, [&] { return r.host.has(1, SocketEvent::Connected); }), "the next peer: socket 1");
	const int c3 = r.sim.peerConnect(80);
	r.run(50);
	check(c3 >= 0 && r.sim.conn(c3).deviceClosed, "a peer while nobody listens: closed");
	const int listening = r.sim.openSockets();
	r.run(2500);
	check(r.sim.openSockets() == listening - 1, "the listening socket closed after 2 s with nobody listening");
	r.chip->close(0);
	r.chip->close(1);
	r.run(20);
	check(r.host.has(0, SocketEvent::Closed) && r.host.has(1, SocketEvent::Closed) && r.sim.openSockets() == 0,
	      "close(): Closed, and every module socket closed");
	check(r.chip->listen(2, 81) && r.chip->listen(3, 82), "two more ports");
	check(r.runUntil(300, [&] { return r.host.has(2, SocketEvent::Listening) && r.host.has(3, SocketEvent::Listening); }), "both listening");
	check(r.chip->listen(4, 83) && r.runUntil(100, [&] { return r.host.has(4, SocketEvent::Failed); }), "a third port: Failed (two at once)");
	check(r.sim.misuse == 0, "no misuse");
	r.misuseReport();
}

static void testDnsTime() {
	std::printf("DNS and time\n");
	Rig r;
	r.sim.dns["broker.example"] = IpAddress(93, 184, 216, 34);
	check(!r.chip->resolve("broker.example"), "resolve() before configure(): false");
	r.start();
	check(r.chip->resolve("broker.example"), "resolve() before an address: taken");
	r.chip->join("home", "correct horse");
	check(r.runUntil(2000, [&] { return r.host.resolvedCount == 1; }) && r.host.resolvedOk && r.host.resolvedIp == IpAddress(93, 184, 216, 34),
	      "answered once up");
	r.chip->resolve("nothing.example");
	check(r.runUntil(200, [&] { return r.host.resolvedCount == 2; }) && !r.host.resolvedOk, "an unknown name: not found");
	r.chip->resolve("10.1.2.3");
	r.run(2);
	check(r.host.resolvedCount == 3 && r.host.resolvedIp == IpAddress(10, 1, 2, 3), "an address: answered at once");
	r.sim.dnsMs = 100;
	r.chip->resolve("nothing.example");
	r.run(10);
	r.chip->resolve("broker.example");
	r.run(300);
	check(r.host.resolvedCount == 4 && r.host.resolvedOk, "a second call replaces the first: one answer, the second's");
	check(!r.chip->resolve(std::string(64, 'a').c_str()) && !r.chip->resolve(""), "too long or empty: false");

	check(r.chip->requestTime("pool.ntp.org"), "requestTime()");
	r.run(3000);
	check(r.host.timeCount == 0, "not synchronised (year 0): asks again");
	r.sim.utcSeconds = 1791000000;   // 2026-10-03 03:20:00 UTC
	check(r.runUntil(1500, [&] { return r.host.timeCount == 1; }) && r.host.timeOk && r.host.timeUnixMs == 1791000000500ull,
	      "then the time, to the second (+500 ms)");
	r.sim.utcSeconds = 0;
	r.chip->requestTime("pool.ntp.org");
	r.run(16000);
	check(r.host.timeCount == 2 && !r.host.timeOk, "never synchronised: not ok after 15 s");
	check(r.sim.misuse == 0, "no misuse");
}

static void testLinkLoss() {
	std::printf("link loss\n");
	Rig r;
	r.sim.servers["192.168.4.10:1"] = SimWinc1500::Policy::Accept;
	r.up();
	r.chip->connect(0, IpAddress(192, 168, 4, 10), 1, 0);
	r.chip->listen(1, 80);
	r.runUntil(300, [&] { return r.host.has(0, SocketEvent::Connected) && r.host.has(1, SocketEvent::Listening); });
	r.sim.apDrops();
	r.run(50);
	check(r.host.has(0, SocketEvent::Failed) && r.host.has(1, SocketEvent::Failed), "the network gone: the connection and the listen Failed");
	check(r.host.addrs.back().ip.isZero(), "address all zeros");
	r.run(20);
	check(r.sim.openSockets() == 0, "nothing left open on the module");
	check(r.sim.misuse == 0, "no misuse");
}

static void testFaults() {
	std::printf("bus faults\n");
	{
		Rig r;
		r.sim.servers["192.168.4.10:1"] = SimWinc1500::Policy::Accept;
		r.up();
		r.chip->connect(0, IpAddress(192, 168, 4, 10), 1, 0);
		r.runUntil(300, [&] { return r.host.has(0, SocketEvent::Connected); });
		const std::vector<uint8_t> in = pattern(5000, 4);
		r.sim.peerSend(0, in.data(), in.size());
		std::vector<uint8_t> early;
		for (int i = 0; i < 40; ++i) {
			r.sim.spiFailNext = (i % 3) == 0;
			r.sim.corruptNextCommand = (i % 5) == 0 ? 1 : 0;
			r.run(3);
			auto v = r.host.takeRx(0);
			early.insert(early.end(), v.begin(), v.end());
		}
		r.host.rx[0].insert(r.host.rx[0].begin(), early.begin(), early.end());
		std::vector<uint8_t> got = r.host.takeRx(0);
		const std::vector<uint8_t> rest = r.drain(0, in.size() - got.size(), 2000);
		got.insert(got.end(), rest.begin(), rest.end());
		check(got == in, "failed transfers and corrupted commands meanwhile: the data whole");
		check(r.chip->stats().spiRetries > 5 && r.chip->stats().failures == 0, "retried after SPI resets, never given up");
		check(r.sim.misuse == 0, "no misuse");
	}
	{
		Rig r;
		r.up();
		r.sim.allocBusyPolls = 30;
		r.chip->resolve("x.example");
		check(r.runUntil(500, [&] { return r.host.resolvedCount == 1; }), "the module slow to hand out a buffer: the request waits, then goes");
		r.sim.allocBusyPolls = -1;
		r.chip->resolve("y.example");
		r.run(2000);
		check(r.chip->stats().allocWaits >= 5 && r.host.resolvedCount == 1, "no buffer at all: put off and tried again");
		r.run(5000);
		check(r.chip->stats().failures == 1 && r.host.hasDev(DeviceEvent::Failed), "for 5 s: the module restarted");
		r.sim.allocBusyPolls = 0;
		check(r.runUntil(8000, [&] { return r.host.countDev(DeviceEvent::LinkUp) == 2; }), "and back: Ready, joined again by itself");
	}
	{
		Rig r;
		r.up();
		r.host.dev.clear();
		r.sim.resetPin(true);   // the module resets behind the driver's back
		r.sim.resetPin(false);
		r.run(8000);
		check(r.host.hasDev(DeviceEvent::Failed), "the module reset: noticed (it takes no requests)");
		check(r.runUntil(5000, [&] { return r.host.hasDev(DeviceEvent::LinkUp); }), "started again and joined");
	}
}

static void testIrq() {
	std::printf("the interrupt\n");
	winc_param_t p;
	p.pollMs = 1000;   // the IRQN pin wired: polling only as a backstop
	Rig r(p);
	r.wireIrq();
	r.sim.servers["192.168.4.10:1"] = SimWinc1500::Policy::Accept;
	check(r.up(), "up, with the interrupt doing the work");
	r.chip->connect(0, IpAddress(192, 168, 4, 10), 1, 0);
	check(r.runUntil(200, [&] { return r.host.has(0, SocketEvent::Connected); }), "Connected");
	const uint32_t t0 = r.now;
	r.sim.peerSend(0, std::string("ping"));
	r.runUntil(100, [&] { return r.host.rx[0].size() == 4; });
	check(r.host.rx[0].size() == 4 && r.now - t0 < 10, "data within a few ms, not pollMs");
	check(r.chip->stats().interrupts > 0, "interrupts counted");
	check(r.sim.misuse == 0, "no misuse");
}

int main() {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	testCrc7();
	testStartup();
	testWifi();
	testTcp();
	testListen();
	testDnsTime();
	testLinkLoss();
	testFaults();
	testIrq();
	std::printf("%s\n", g_failures ? "FAILED" : "all passed");
	return g_failures ? 1 : 0;
}
