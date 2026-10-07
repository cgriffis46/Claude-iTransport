/*
 * bringup.cpp — brings the network hardware up one step at a time,
 * saying on the log what each step found and, when one fails, what to
 * look at. Then it starts the test services and logs statistics.
 *
 *   W5500:  hardware reset; VERSIONR, register write/read-back and a
 *           2 KB DMA transfer at each SPI speed, slowest first, to find
 *           the fastest the wiring carries; INT idle level; PHY link;
 *           then the driver, DHCP, and the services.
 *   ESP-AT: the driver's own start-up (reset pin, AT, AT+GMR), joining
 *           the network, then the echo service.
 */

#include <stdio.h>
#include "main.h"
#include "cmsis_os2.h"
#include "FreeRTOS.h"
#include "board.h"
#include "config.h"
#include "log.h"
#include "services.h"
#include "Stm32HalSpiBlockTransport.h"
#include "Stm32HalUartTransport.h"
#include "W5500.h"
#include "W5500Probe.h"
#include "EspAt.h"
#include "xEthernet.h"
#include "xWifi.h"

namespace {

const char *ipStr(const IpAddress &a, char *buf) {
	snprintf(buf, 16, "%u.%u.%u.%u", a.b[0], a.b[1], a.b[2], a.b[3]);
	return buf;
}

unsigned prefixLen(const IpAddress &m) {
	unsigned n = 0;
	for (int i = 0; i < 4; ++i) for (int b = 7; b >= 0; --b) n += (m.b[i] >> b) & 1;
	return n;
}

void logAddress(const char *tag, const NetConfig &a) {
	char ip[16], gw[16], dns[16];
	log_printf("[%s] address %s/%u, gateway %s, dns %s (%s)", tag, ipStr(a.ip, ip), prefixLen(a.subnet),
	           ipStr(a.gateway, gw), ipStr(a.dns, dns), a.dhcp ? "DHCP" : "static");
}

void netThread(void *arg) {
	static_cast<xNetInterface *>(arg)->run();
}

void startNetThread(xNetInterface &net, const char *name) {
	osThreadAttr_t a = {};
	a.name = name;
	a.stack_size = 1024;
	a.priority = osPriorityAboveNormal;	// above the threads it serves
	osThreadNew(netThread, &net, &a);
}

// Waits for an interface event, saying so every few seconds.
template <typename F>
bool waitFor(const char *tag, const char *what, uint32_t ms, F done) {
	const uint32_t t0 = osKernelGetTickCount();
	uint32_t said = t0;
	while (!done(500)) {
		const uint32_t now = osKernelGetTickCount();
		if (now - t0 >= ms) return false;
		if (now - said >= 3000) {
			log_printf("[%s] still waiting for %s...", tag, what);
			said = now;
		}
	}
	return true;
}

#if BRINGUP_ETH
typedef W5500::w5500<Stm32HalSpiBlockTransport> Chip;
Chip *g_chip = nullptr;
xEthernet *g_eth = nullptr;
ServiceStats g_ethStats = {};

const char *prescName(uint32_t p) {
	switch (p) {
	case SPI_BAUDRATEPRESCALER_2: return "/2";
	case SPI_BAUDRATEPRESCALER_4: return "/4";
	case SPI_BAUDRATEPRESCALER_8: return "/8";
	case SPI_BAUDRATEPRESCALER_16: return "/16";
	case SPI_BAUDRATEPRESCALER_32: return "/32";
	case SPI_BAUDRATEPRESCALER_64: return "/64";
	case SPI_BAUDRATEPRESCALER_128: return "/128";
	default: return "/256";
	}
}

// The W5500 and its wiring, on a bare transport, before the driver.
// Leaves SPI1 at the fastest speed that passed. false: nothing passed.
bool ethProbe(osMutexId_t spiMutex) {
	static Stm32HalSpiBlockTransport spi(&hspi1, W5500_CS_GPIO_Port, W5500_CS_Pin, spiMutex);
	W5500::w5500_probe probe(spi, board_ms, 200);
	typedef W5500::w5500_probe::Result R;
	static uint8_t scratch[2048];

	log_printf("[eth] W5500: pulsing RSTn (PA3)");
	board_w5500_reset();

	const uint32_t speeds[] = {SPI_BAUDRATEPRESCALER_256, SPI_BAUDRATEPRESCALER_32,
	                           SPI_BAUDRATEPRESCALER_8, SPI_BAUDRATEPRESCALER_4, SPI_BAUDRATEPRESCALER_2};
	uint32_t best = 0;
	bool any = false;
	for (uint32_t p : speeds) {
		if (p < SPI_FASTEST_PRESCALER) break;	// faster than allowed
		board_spi_set_prescaler(p);
		uint8_t v = 0;
		R r = probe.version(v);
		if (r == R::Ok) r = probe.writeRead();
		if (r == R::Ok) r = probe.bufferTest(scratch, sizeof scratch);
		const unsigned long khz = board_spi_hz() / 1000;
		if (r == R::Ok) {
			log_printf("[eth] SPI %s (%lu kHz): VERSIONR 0x%02X, write/read-back, 2 KB DMA transfer: ok", prescName(p), khz, v);
			best = p;
			any = true;
		} else {
			log_printf("[eth] SPI %s (%lu kHz): FAILED: %s", prescName(p), khz, W5500::w5500_probe::describe(r));
			if (r == R::Corrupt) log_printf("[eth]   first bad byte at offset %u", static_cast<unsigned>(probe.badAt()));
			if (r == R::WrongVersion || r == R::ReadsZero || r == R::ReadsOnes) log_printf("[eth]   VERSIONR read 0x%02X", v);
			if (!any) {
				log_printf("[eth] the W5500 doesn't answer at the slowest speed: a wiring or power problem, not speed.");
				log_printf("[eth]   3.3 V and GND to the module; SCK PA5 [A4], MISO PA6 [A5], MOSI PA7 [A6], CS PA4 [A3], RSTn PA3 [A2].");
				return false;
			}
			break;	// faster won't do better
		}
	}
	board_spi_set_prescaler(best);
	log_printf("[eth] running SPI at %lu kHz", static_cast<unsigned long>(board_spi_hz() / 1000));
	if (!(hspi1.hdmarx && hspi1.hdmatx)) log_printf("[eth] warning: SPI1 has no DMA linked; large transfers go by interrupt");

	if (HAL_GPIO_ReadPin(W5500_INT_GPIO_Port, W5500_INT_Pin) == GPIO_PIN_SET) {
		log_printf("[eth] INT (PA1 [A1]) idles high: ok");
	} else {
		log_printf("[eth] warning: INT (PA1 [A1]) is low with nothing pending: not connected? The driver still works by polling.");
	}

	uint8_t phy = 0;
	if (probe.phy(phy) == R::Ok) {
		if (phy & 1) log_printf("[eth] PHY: link up, %s Mbps, %s duplex", (phy & 2) ? "100" : "10", (phy & 4) ? "full" : "half");
		else log_printf("[eth] PHY: no link yet (cable? switch port? link LEDs on the jack?)");
	}
	return true;
}

void ethStart() {
	static osMutexId_t spiMutex = osMutexNew(nullptr);
	if (!ethProbe(spiMutex)) {
		log_printf("[eth] stopped here. Fix the wiring and reset the board.");
		return;
	}

	const uint32_t uid = HAL_GetUIDw0() ^ HAL_GetUIDw1() ^ HAL_GetUIDw2();
	static W5500::w5500_param_t param = [uid] {
		W5500::w5500_param_t p;
		for (int i = 0; i < 8; ++i) p.rxBufKb[i] = p.txBufKb[i] = (i < 4 || i == W5500::w5500_dhcp_socket) ? 2 : 0;
		p.pollMs = 100;			// INT is wired: polling is a backstop
		p.dhcpSeed = uid;
		return p;
	}();
	static Chip chip(param, &hspi1, W5500_CS_GPIO_Port, W5500_CS_Pin, spiMutex);
	xNetInterface::Config cfg;
	cfg.maxSockets = 4;			// echo, discard, chargen, and one spare
	static xEthernet eth(chip, cfg);
	g_chip = &chip;

	NetConfig net;
	net.mac = MacAddress(0x02, 0x08, 0xDC, static_cast<uint8_t>(uid >> 16), static_cast<uint8_t>(uid >> 8),
	                     static_cast<uint8_t>(uid));
	net.dhcp = ETH_DHCP;
	if (!net.dhcp) {
		net.ip = IpAddress(ETH_STATIC_IP);
		net.gateway = IpAddress(ETH_STATIC_GW);
		net.subnet = IpAddress(ETH_STATIC_MASK);
	}
	log_printf("[eth] MAC 02:08:DC:%02X:%02X:%02X (from the MCU's unique ID)", net.mac.b[3], net.mac.b[4], net.mac.b[5]);
	if (!eth.begin(net)) {
		log_printf("[eth] begin() failed: FreeRTOS heap too small");
		return;
	}
	g_eth = &eth;	// the EXTI callback may use it from here
	startNetThread(eth, "eth");

	const uint32_t t0 = osKernelGetTickCount();
	if (!waitFor("eth", "the driver", 5000, [&](uint32_t ms) { return eth.waitReady(ms); })) {
		log_printf("[eth] driver not ready after 5 s; failures so far: %lu", static_cast<unsigned long>(chip.stats().failures));
		return;
	}
	log_printf("[eth] driver ready in %lu ms", static_cast<unsigned long>(osKernelGetTickCount() - t0));
	if (!waitFor("eth", "a link", 30000, [&](uint32_t ms) { return eth.waitLinkUp(ms); })) {
		log_printf("[eth] no link after 30 s: carrying on; it comes up when a cable is plugged in");
	}
	if (!waitFor("eth", net.dhcp ? "a DHCP lease" : "the address", 30000, [&](uint32_t ms) { return eth.waitAddress(ms); })) {
		log_printf("[eth] no address after 30 s%s", net.dhcp ? ": is there a DHCP server on this network? Still asking." : "");
	} else {
		log_printf("[eth] address in %lu ms", static_cast<unsigned long>(osKernelGetTickCount() - t0));
		logAddress("eth", eth.address());
	}
	services_start(eth, "eth", g_ethStats, SERVICE_ALL);
	log_printf("[eth] services: echo :7, discard :9, chargen :19");
}
#endif

#if BRINGUP_WIFI
typedef ESPAT::espat<Stm32HalUartTransport> Esp;
Esp *g_esp = nullptr;
ServiceStats g_wifiStats = {};

void wifiStart() {
	static ESPAT::espat_param_t param = [] {
		ESPAT::espat_param_t p;
		p.hardReset = [](bool asserted) { board_esp_enable(!asserted); };
		return p;
	}();
	static Esp esp(param, &huart1);
	xNetInterface::Config cfg;
	cfg.maxSockets = 2;			// echo and one spare
	static xWifi wifi(esp, cfg);
	g_esp = &esp;

	NetConfig net;
	net.dhcp = true;
	if (!wifi.begin(net)) {
		log_printf("[wifi] begin() failed: FreeRTOS heap too small");
		return;
	}
	startNetThread(wifi, "wifi");
	log_printf("[wifi] ESP-AT module on USART1 at %lu baud, EN on PA8 [D9]", static_cast<unsigned long>(ESP_BAUD));

	uint32_t t0 = osKernelGetTickCount();
	if (!waitFor("wifi", "the module", 15000, [&](uint32_t ms) { return wifi.waitReady(ms); })) {
		const ESPAT::espat_stats_t st = esp.stats();
		log_printf("[wifi] the module never answered \"AT\" (%lu commands sent, %lu failures).",
		           static_cast<unsigned long>(st.commands), static_cast<unsigned long>(st.failures));
		log_printf("[wifi]   TX/RX crossed? module TX -> PA10 [D0], module RX <- PA9 [D1]. Baud %lu? EN high? "
		           "3.3 V supply good for 500 mA peaks?", static_cast<unsigned long>(ESP_BAUD));
		return;
	}
	log_printf("[wifi] module ready in %lu ms: %s", static_cast<unsigned long>(osKernelGetTickCount() - t0),
	           esp.firmware()[0] ? esp.firmware() : "(no AT+GMR version line)");

	t0 = osKernelGetTickCount();
	log_printf("[wifi] joining \"%s\"...", WIFI_SSID);
	if (!wifi.join(WIFI_SSID, WIFI_PASS, 30000)) {
		log_printf("[wifi] join failed: SSID/passphrase (config.h, or -DWIFI_SSID/-DWIFI_PASS), 2.4 GHz network, in range?");
		return;
	}
	if (wifi.waitAddress(5000)) {
		log_printf("[wifi] joined in %lu ms", static_cast<unsigned long>(osKernelGetTickCount() - t0));
		logAddress("wifi", wifi.address());
	}
	services_start(wifi, "wifi", g_wifiStats, SERVICE_ECHO);
	log_printf("[wifi] services: echo :7");
}
#endif

void statsLine(uint32_t upMs) {
	log_printf("[stats] up %lu s, heap free %u (lowest %u)", static_cast<unsigned long>(upMs / 1000),
	           static_cast<unsigned>(xPortGetFreeHeapSize()), static_cast<unsigned>(xPortGetMinimumEverFreeHeapSize()));
#if BRINGUP_ETH
	if (g_chip && g_eth) {
		const W5500::w5500_stats_t s = g_chip->stats();
		char ip[16];
		log_printf("[stats] eth: link %s, %s, spi %lu xfers, %lu restarts, %lu irqs, rx %lu B, tx %lu B, %lu connections",
		           g_eth->linkUp() ? "up" : "DOWN", ipStr(g_eth->address().ip, ip), static_cast<unsigned long>(s.transfers),
		           static_cast<unsigned long>(s.failures), static_cast<unsigned long>(s.interrupts),
		           static_cast<unsigned long>(s.rxBytes), static_cast<unsigned long>(s.txBytes),
		           static_cast<unsigned long>(g_ethStats.connections));
	}
#endif
#if BRINGUP_WIFI
	if (g_esp) {
		const ESPAT::espat_stats_t s = g_esp->stats();
		log_printf("[stats] wifi: %lu commands, %lu timeouts, %lu restarts, %lu uart overflows, rx %lu B, tx %lu B, %lu connections",
		           static_cast<unsigned long>(s.commands), static_cast<unsigned long>(s.timeouts),
		           static_cast<unsigned long>(s.failures), static_cast<unsigned long>(s.rxOverflows),
		           static_cast<unsigned long>(s.rxBytes), static_cast<unsigned long>(s.txBytes),
		           static_cast<unsigned long>(g_wifiStats.connections));
	}
#endif
}

void bringupThread(void *) {
	log_printf("%s", "");
	log_printf("=== iNetTransport bring-up: NUCLEO-L432KC ===");
	log_printf("SYSCLK %lu MHz, LSE %s, reset cause: %s", static_cast<unsigned long>(HAL_RCC_GetSysClockFreq() / 1000000),
	           board_lse_ok() ? "running (MSI trimmed)" : "not running (MSI untrimmed)", board_reset_cause());
	log_printf("building: Ethernet (W5500) %s, Wi-Fi (ESP-AT) %s", BRINGUP_ETH ? "yes" : "no", BRINGUP_WIFI ? "yes" : "no");

#if BRINGUP_ETH
	ethStart();
#endif
#if BRINGUP_WIFI
	wifiStart();
#endif

	// Heartbeat on LD3, statistics every STATS_PERIOD_MS.
	const uint32_t t0 = osKernelGetTickCount();
	uint32_t lastStats = t0;
	for (;;) {
		osDelay(500);
		HAL_GPIO_TogglePin(LED_GPIO_Port, LED_Pin);
		const uint32_t now = osKernelGetTickCount();
		if (now - lastStats >= STATS_PERIOD_MS) {
			lastStats = now;
			statsLine(now - t0);
		}
	}
}

} // namespace

extern "C" void HAL_GPIO_EXTI_Callback(uint16_t pin) {
#if BRINGUP_ETH
	if (pin == W5500_INT_Pin && g_eth != nullptr) g_eth->interruptFromIsr();
#else
	(void)pin;
#endif
}

void bringup_start() {
	osThreadAttr_t a = {};
	a.name = "bringup";
	a.stack_size = 2048;
	a.priority = osPriorityNormal;
	osThreadNew(bringupThread, nullptr, &a);
}
