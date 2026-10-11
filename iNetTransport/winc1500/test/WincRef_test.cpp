/*
 * WincRef_test.cpp
 *
 * Microchip's own ATWINC1500 host driver (19.5.2, the protocol reference
 * WincProtocol.h comes from) run on the PC against our simulated module,
 * test/sim/SimWinc1500.h. The simulator is what the other tests check our
 * driver against, so here it is checked against the real driver: start-up
 * (the SPI byte by byte, CRC on then off, the boot handshake), the
 * firmware version, a join with DHCP, RSSI, the time, a TCP connection
 * both ways, a listening socket, and DNS. The simulator counts anything
 * Microchip's driver does that it doesn't expect (misuse), and that must
 * stay 0. ref/ref_layout.cpp also checks WincProtocol.h's numbers against
 * Microchip's headers at compile time.
 *
 * Built by CMake when WINC_REF_DIR points at Arduino's WiFi101 library
 * (git clone https://github.com/arduino-libraries/WiFi101), whose src/
 * holds the driver. The build copies its bsp/include/nm_bsp.h with uint32
 * and sint32 made "int": Microchip's "long" is 8 bytes on a 64-bit PC,
 * which would change the message layouts. Its socket functions (close,
 * connect, send...) are renamed winc_* with -D, as they'd otherwise
 * replace the C library's on the PC. Its sources are built without
 * AddressSanitizer: hif_send() writes 8 bytes from its 4-byte header
 * struct (m2m_hif.c; harmless on the module, which ignores those 4
 * bytes; our driver writes zeros there). This file supplies the bus
 * (nm_bus_ioctl) and the board (nm_bsp_*), over the simulator: the
 * driver's sleeps move the simulated clock.
 */

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "SimWinc1500.h"

extern "C" {
#include "bus_wrapper/include/nm_bus_wrapper.h"
#include "bsp/include/nm_bsp.h"

int ref_init(void);
int ref_firmware(uint8 *maj, uint8 *min, uint8 *patch);
void ref_events(void);
int ref_join(const char *ssid, const char *pass);
int ref_rssi(void);
int ref_time(void);
int ref_tcp_socket(void);
int ref_connect(int s, const uint8 ip[4], uint16 port);
int ref_recv(int s);
int ref_send(int s, const uint8 *d, uint16 n);
int ref_close(int s);
int ref_bind(int s, uint16 port);
int ref_listen(int s);
int ref_resolve(const char *name);
}

static int g_failures = 0;
static void check(bool ok, const char *what) {
	std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok) ++g_failures;
}

// ---- the platform Microchip's driver runs on: the simulator ----
static SimWinc1500 *g_sim = nullptr;
static uint32_t g_now = 1000;
static tpfNmBspIsr g_isr = nullptr;
static bool g_irqOn = true;

static void advance(uint32_t ms) {
	for (uint32_t i = 0; i < ms; ++i) g_sim->tick(++g_now);
}

extern "C" {
// Arduino's bus wrapper: block transfers in pieces of at most 256 - 8 bytes.
tstrNmBusCapabilities egstrNmBusCapabilities = {256};
sint8 nm_bus_init(void *) { return M2M_SUCCESS; }
sint8 nm_bus_deinit(void) { return M2M_SUCCESS; }
sint8 nm_bus_reinit(void *) { return M2M_SUCCESS; }
sint8 nm_bus_ioctl(uint8 cmd, void *param) {
	if (cmd != NM_BUS_IOCTL_RW) return M2M_ERR_BUS_FAIL;
	tstrNmSpiRw *rw = static_cast<tstrNmSpiRw *>(param);
	// One call is one chip-select, as on Arduino's boards. The driver's old
	// SPI path only ever writes or reads, never both.
	if (rw->pu8InBuf && rw->pu8OutBuf) return M2M_ERR_BUS_FAIL;
	if (rw->pu8InBuf) g_sim->spiWrite(rw->pu8InBuf, rw->u16Sz);
	else g_sim->spiRead(rw->pu8OutBuf, rw->u16Sz);
	return M2M_SUCCESS;
}
sint8 nm_bsp_init(void) { return M2M_SUCCESS; }
sint8 nm_bsp_deinit(void) { return M2M_SUCCESS; }
void nm_bsp_reset(void) {
	g_sim->resetPin(true);
	advance(100);
	g_sim->resetPin(false);
	advance(100);
}
void nm_bsp_sleep(uint32 ms) { advance(ms); }
void nm_bsp_register_isr(tpfNmBspIsr isr) { g_isr = isr; }
void nm_bsp_interrupt_ctrl(uint8 enable) { g_irqOn = enable != 0; }

// ---- what it hears ----
static int s_state = -1, s_err = -1, s_rssi = 0, s_year = 0, s_connSock = -1, s_connErr = 99;
static int s_sent = 0, s_bound = 99, s_listening = 99, s_acceptSock = -1, s_closedSock = -1;
static uint8 s_ip[4] = {0, 0, 0, 0}, s_resolved[4] = {1, 1, 1, 1};
static std::string s_resolvedName;
static std::vector<uint8_t> s_rx[7];
void ref_on_wifi_state(int state, int err) { s_state = state; s_err = err; }
void ref_on_dhcp(const uint8 ip[4]) { std::memcpy(s_ip, ip, 4); }
void ref_on_rssi(int rssi) { s_rssi = rssi; }
void ref_on_time(int year, int, int, int, int, int) { s_year = year; }
void ref_on_connect(int sock, int err) { s_connSock = sock; s_connErr = err; }
void ref_on_send(int, int sent) { s_sent += sent; }
void ref_on_recv(int sock, const uint8 *d, int len) {
	if (len > 0) s_rx[sock].insert(s_rx[sock].end(), d, d + len);
	else s_closedSock = sock;
}
void ref_on_bind(int, int status) { s_bound = status; }
void ref_on_listen(int, int status) { s_listening = status; }
void ref_on_accept(int, int sock) { s_acceptSock = sock; }
void ref_on_resolve(const char *name, const uint8 ip[4]) { s_resolvedName = name; std::memcpy(s_resolved, ip, 4); }
}

// The application's loop: time passes, the module's interrupt reaches
// the driver, the driver handles its events.
template <typename F>
static bool pumpUntil(uint32_t ms, F cond) {
	for (uint32_t i = 0; i < ms; ++i) {
		if (cond()) return true;
		advance(1);
		ref_events();
	}
	return cond();
}

int main() {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	SimWinc1500 sim;
	g_sim = &sim;
	sim.respLatency = 2;   // Microchip's driver polls through these too
	sim.onIrq = []() { if (g_isr && g_irqOn) g_isr(); };
	sim.aps.push_back({"home", "correct horse"});
	sim.servers["192.168.4.10:1883"] = SimWinc1500::Policy::Accept;
	sim.dns["broker.example"] = IpAddress(192, 168, 4, 10);
	sim.utcSeconds = 1791000000;

	std::printf("Microchip's driver on the simulated module\n");
	check(ref_init() == M2M_SUCCESS, "m2m_wifi_init(): reset, SPI CRC off, boot handshake, firmware up");
	check(!sim.crcOn() && sim.hostVersion == 0x13521352 && sim.irqEnabled(), "the same register values as ours");
	uint8 maj = 0, min = 0, patch = 0;
	check(ref_firmware(&maj, &min, &patch) == M2M_SUCCESS && maj == 19 && min == 6 && patch == 1,
	      "m2m_wifi_get_firmware_version(): 19.6.1, from the simulator's tstrM2mRev");

	check(ref_join("home", "correct horse") == M2M_SUCCESS, "m2m_wifi_connect()");
	check(pumpUntil(2000, [] { return s_ip[0] != 0; }) && s_state == 1, "connected, then the DHCP address");
	check(s_ip[0] == 192 && s_ip[1] == 168 && s_ip[2] == 4 && s_ip[3] == 23, "192.168.4.23, in network order");
	check(sim.lastSsid == "home" && sim.lastPass == "correct horse" && sim.lastSecType == 2, "the simulator read its tstrM2mWifiConnect");

	ref_rssi();
	check(pumpUntil(200, [] { return s_rssi != 0; }) && s_rssi == -55, "RSSI");
	ref_time();
	check(pumpUntil(200, [] { return s_year != 0; }) && s_year == 2026, "system time: 2026");

	std::printf("TCP\n");
	const int s = ref_tcp_socket();
	const uint8 broker[4] = {192, 168, 4, 10};
	check(s >= 0 && ref_connect(s, broker, 1883) == 0, "socket(), connect()");
	check(pumpUntil(500, [] { return s_connErr != 99; }) && s_connSock == s && s_connErr == 0, "connected");
	ref_recv(s);
	std::vector<uint8_t> out(3000);
	for (size_t i = 0; i < out.size(); ++i) out[i] = static_cast<uint8_t>(i * 13 + 5);
	size_t at = 0;
	while (at < out.size()) {
		const uint16_t n = static_cast<uint16_t>(out.size() - at < 1400 ? out.size() - at : 1400);
		if (ref_send(s, &out[at], n) == 0) at += n;
		pumpUntil(5, [] { return false; });
	}
	pumpUntil(200, [&] { return s_sent == static_cast<int>(out.size()); });
	check(sim.fromDevice(0) == out && s_sent == 3000, "3000 bytes sent (data at offset 80), each SEND answered");
	std::vector<uint8_t> in(2500);
	for (size_t i = 0; i < in.size(); ++i) in[i] = static_cast<uint8_t>(i * 7 + 1);
	sim.peerSend(0, in.data(), in.size());
	check(pumpUntil(500, [&] { return s_rx[s].size() >= in.size(); }) && s_rx[s] == in,
	      "2500 bytes received (in 1400-byte replies, data at the reply's offset)");
	sim.peerClose(0);
	check(pumpUntil(200, [&] { return s_closedSock == s; }), "the peer's close: a RECV reply with status 0");
	ref_close(s);
	pumpUntil(20, [] { return false; });
	check(sim.conn(0).deviceClosed, "close() reached the simulator");

	std::printf("listening\n");
	const int ls = ref_tcp_socket();
	ref_bind(ls, 80);
	check(pumpUntil(200, [] { return s_bound != 99; }) && s_bound == 0, "bind()");
	ref_listen(ls);
	check(pumpUntil(200, [] { return s_listening != 99; }) && s_listening == 0, "listen()");
	const int c = sim.peerConnect(80);
	check(c >= 0 && pumpUntil(200, [] { return s_acceptSock >= 0; }), "a peer connects: ACCEPT, with the module's socket number");
	s_rx[s_acceptSock].clear();
	ref_recv(s_acceptSock);
	sim.peerSend(c, "GET / HTTP/1.0\r\n\r\n");
	check(pumpUntil(200, [&] { return s_rx[s_acceptSock].size() == 18; }), "its request received");

	std::printf("DNS\n");
	ref_resolve("broker.example");
	check(pumpUntil(200, [] { return !s_resolvedName.empty(); }) && s_resolvedName == "broker.example" &&
	          s_resolved[0] == 192 && s_resolved[3] == 10,
	      "gethostbyname(): the name and address back");

	check(sim.misuse == 0, "the simulator saw nothing it didn't expect from Microchip's driver");
	for (auto &m : sim.misuseWhat) std::printf("    misuse: %s\n", m.c_str());
	std::printf("  (%d SPI commands, %d HIF messages in, %d out)\n", sim.commands, sim.messagesIn, sim.messagesOut);
	std::printf("%s\n", g_failures ? "FAILED" : "all passed");
	return g_failures ? 1 : 0;
}
