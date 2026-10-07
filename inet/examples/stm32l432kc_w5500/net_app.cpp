/*
 * net_app.cpp
 *
 *  W5500 on an STM32L432KC (NUCLEO-L432KC) under FreeRTOS: a TCP echo
 *  server on port 7, with its address from DHCP. The smallest version;
 *  ../stm32l432kc_bringup is a complete firmware with diagnostics. Drop this file into a CubeMX/CubeIDE project and
 *  call net_app_start() from main() after the MX_*_Init() calls and
 *  osKernelInitialize(), before osKernelStart().
 *
 *  CubeMX
 *    SPI1       Full-Duplex Master, 8 bit, Motorola, MSB first,
 *               CPOL Low / CPHA 1 Edge (mode 0), NSS Software.
 *               Prescaler 4 -> 20 MHz at 80 MHz SYSCLK.
 *               PA5 SCK, PA6 MISO, PA7 MOSI.
 *               DMA: SPI1_RX DMA1 Channel 2, SPI1_TX DMA1 Channel 3,
 *               Normal, byte. NVIC: SPI1 global interrupt and both DMA
 *               channel interrupts enabled, "uses FreeRTOS functions".
 *    PA4        GPIO_Output, label W5500_CS, initial level High.
 *    PA1        GPIO_EXTI1, label W5500_INT, falling edge, pull-up.
 *               NVIC: EXTI line1 enabled, "uses FreeRTOS functions".
 *    (W5500 RSTn: tie high, or to a GPIO held high.)
 *    FREERTOS   CMSIS_V2. TOTAL_HEAP_SIZE at least 16 KB: the four
 *               sockets' stream buffers below take 8 KB of it.
 *
 *  NUCLEO-L432KC: solder bridges SB16/SB18 join PA5/PA6 (A4/A5) to
 *  PB7/PB6 (D4/D5) for Arduino Nano compatibility. Leave PB6/PB7
 *  unconfigured, or open the bridges (see UM1956).
 *
 *  Include paths: itransport/inc, itransport/hw/stm32/inc,
 *  itransport/hw/freertos/inc, inet/inc, inet/hw/freertos/inc,
 *  inet/w5500/inc. Sources: itransport/src/{BusTransport,
 *  SpiBlockTransport}.cpp, itransport/hw/stm32/src/{
 *  Stm32HalSpiBlockTransport,Stm32SpiItCallbacks}.cpp,
 *  inet/hw/freertos/src/{xNetInterface,xClient}.cpp,
 *  inet/dhcp/src/DhcpClient.cpp; include path inet/dhcp/inc too.
 */

#include "main.h"
#include "cmsis_os2.h"
#include "Stm32HalSpiBlockTransport.h"
#include "W5500.h"
#include "xEthernet.h"
#include "xClient.h"

extern SPI_HandleTypeDef hspi1;

namespace {

xEthernet *g_eth = nullptr; // for the EXTI callback

void netThread(void *arg) {
	static_cast<xEthernet *>(arg)->run(); // never returns
}

// One connection at a time, echoed back. read() sleeps until data
// arrives; the 10 s timeout just bounds an idle connection.
void echoThread(void *arg) {
	xEthernet &eth = *static_cast<xEthernet *>(arg);
	eth.waitAddress(xNetInterface::kForever);	// the DHCP lease

	xClient client(eth);
	uint8_t buf[256];
	for (;;) {
		if (!client.listen(7)) {
			osDelay(1000);
			continue;
		}
		if (!client.accept(xNetInterface::kForever)) continue;

		for (;;) {
			const int32_t n = client.read(buf, sizeof buf, 10000);
			if (n < 0) break;		// peer closed
			if (n == 0) break;		// idle too long
			if (client.write(buf, static_cast<size_t>(n), 1000) != n) break;
		}
		client.stop();
	}
}

} // namespace

extern "C" void net_app_start(void) {
	// Created once, here rather than as globals: the mutex needs the
	// kernel initialised, and the transport drives CS in its constructor.
	static osMutexId_t spi1Mutex = osMutexNew(nullptr);

	static W5500::w5500_param_t param = [] {
		W5500::w5500_param_t p;
		for (int i = 0; i < 8; ++i) {	// 2 KB each for sockets 0-3 and DHCP's socket 7
			const bool used = i < 4 || i == W5500::w5500_dhcp_socket;
			p.rxBufKb[i] = used ? 2 : 0;
			p.txBufKb[i] = used ? 2 : 0;
		}
		p.dhcpSeed = HAL_GetUIDw0() ^ HAL_GetUIDw1() ^ HAL_GetUIDw2();
		p.pollMs = 100;					// INT is wired: polling is only a backstop
		return p;
	}();
	static W5500::w5500<Stm32HalSpiBlockTransport> chip(
		param, &hspi1, W5500_CS_GPIO_Port, W5500_CS_Pin, spi1Mutex);

	static xEthernet eth(chip);			// default Config: 4 sockets, 1 KB buffers each way
	g_eth = &eth;

	NetConfig net;
	net.mac  = MacAddress(0x02, 0x08, 0xDC, 0x00, 0x00, 0x01);	// locally administered: make it unique per board
	net.dhcp = true;	// or false, with ip, subnet and gateway set here
	eth.begin(net);

	static const osThreadAttr_t netAttr = {
		.name = "net", .attr_bits = 0, .cb_mem = nullptr, .cb_size = 0,
		.stack_mem = nullptr, .stack_size = 1024, .priority = osPriorityAboveNormal,
		.tz_module = 0, .reserved = 0 };
	static const osThreadAttr_t echoAttr = {
		.name = "echo", .attr_bits = 0, .cb_mem = nullptr, .cb_size = 0,
		.stack_mem = nullptr, .stack_size = 1024, .priority = osPriorityNormal,
		.tz_module = 0, .reserved = 0 };
	osThreadNew(netThread, &eth, &netAttr);
	osThreadNew(echoThread, &eth, &echoAttr);
}

// The W5500's INT line. If the project already has an EXTI callback,
// add the W5500_INT case to it instead.
extern "C" void HAL_GPIO_EXTI_Callback(uint16_t pin) {
	if (pin == W5500_INT_Pin && g_eth != nullptr) g_eth->interruptFromIsr();
}
