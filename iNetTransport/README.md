# iNetTransport

Network interfaces on top of itransport: `xEthernet`, `xWifi` and `xClient`
for FreeRTOS, with two chip drivers underneath. One is the WIZnet W5500
(Ethernet over SPI). The other is an Espressif module running ESP-AT
firmware (Wi-Fi over a UART). There is also a DHCP client. Target:
STM32L432KC.

## Layers

```
 user threads   xClient  connect / listen / accept / read / write / stop
                   |       (each sleeps on its socket's event group, with a timeout)
 hw/freertos    xEthernet / xWifi  ->  xNetInterface
                   |       driver thread; FreeRTOS queue (inbox),
                   |       per-socket stream buffers + event group
 inc/           iNetDevice / iEthernetDevice / iWifiDevice
                   |       the seam: any chip, any bus
 w5500/         w5500<TTransport>        espat/   espat<TTransport>
                   |   + dhcp/DhcpClient             |   (the module does DHCP)
 itransport     iBlockTransport (SPI, DMA)          iTransport (UART stream)
```

The bus is the chip driver's business, not the interface's. A chip on SPI
uses `iBlockTransport` (W5500, ATWINC1500). A module on a UART uses the
stream `iTransport` (ESP-AT). A PHY on MII/RMII would sit behind the MCU's
Ethernet MAC and a host stack such as lwIP. All of them implement
`iNetDevice`, so `xEthernet`, `xWifi` and `xClient` don't change. The
STM32L432 has no Ethernet MAC, so MII/RMII doesn't apply to this target.

## Threads and blocking

- **One driver thread** per interface owns the chip driver and is the only
  thread that touches it. It runs `eth.run()`: it handles requests from its
  inbox (a FreeRTOS queue), calls the device's `poll()`, then sleeps on the
  inbox for as long as `poll()` says it can. Any of these wakes it at once:
  - a request, or a write (as a "kick");
  - the W5500's INT pin (`interruptFromIsr()`);
  - a line ending on the ESP's UART (`iNetDeviceHost::wakeFromIsr()`).
- **Data** goes through two stream buffers per socket: the driver thread
  writes `rx` and reads `tx`, and the socket's user thread does the reverse.
  That is one writer and one reader each, so no locks are needed.
- **Sleeping**: `client.read(buf, len, timeoutMs)` sleeps on the socket's
  event group until bytes arrive, the connection closes, or the timeout runs
  out. The event group also covers what a stream buffer can't signal, such as
  the peer closing while a reader sleeps. `write`, `connect`, `accept`,
  `flush` and `xWifi::join` sleep the same way.
- **Flow control**: when a reader falls behind, the driver stops taking
  bytes off the chip. The W5500's TCP window closes; the ESP holds the data
  in passive receive mode. The next `read()` that makes room wakes the driver.

## Addresses: static or DHCP

Set `NetConfig::dhcp` and the address comes from a DHCP server. Otherwise
`ip`, `subnet` and `gateway` are used as given. Either way,
`waitAddress()`/`address()` give the address in use. Connects wait for an
address, and an address change drops the connections that were on the old
one.

- **W5500**: `dhcp/DhcpClient` is a DHCP client as pure logic: packets in,
  packets out, lease timers. The W5500 driver runs it on socket 7 in UDP
  mode (`w5500_param_t::dhcp`, on by default). The interface then gets
  sockets 0 to 6. It covers DISCOVER/OFFER/REQUEST/ACK with backoff,
  renewing at T1 (unicast) and rebinding at T2 (broadcast), expiry, NAK, and
  a fresh check when the cable comes back.
- **ESP-AT**: the module does DHCP itself. The driver reads the result back
  with `AT+CIPSTA?` and `AT+CIPDNS?`.

## W5500 driver

`w5500<TTransport>` works like the sensor drivers. Each register access is
one state that issues the transfer and one that waits for it to land, and
it never waits on the chip. It doesn't call `osDelay()`. Instead `poll()`
returns how long it can be left, so the driver thread stays responsive.

- Start-up: soft reset, VERSIONR check, address registers in one burst,
  RTR/RCR, per-socket buffer sizes, socket interrupts on, PHY link.
- Sockets: TCP client and server (each socket is one connection), graceful
  close with a forced CLOSE after a timeout.
- Data: up to 1 KB per SPI transfer (by DMA); several chunks per SEND; one
  SEND in flight; RSR/FSR read until two reads agree.
- With INT wired, SIR is read when the pin fires. Without it, SIR is polled
  every `pollMs`.
- Errors: a failed transfer or a chip that doesn't answer reports every
  socket Failed, backs off, and starts again from reset.
- `W5500Probe.h`: blocking wiring checks for bring-up, run before the driver.

## ESP-AT driver

`espat<TTransport>` drives an ESP32, ESP32-C3 or ESP8266 running ESP-AT
firmware, over any `iTransport` (`Stm32HalUartTransport` on the L432).

- One AT command at a time; `poll()` never waits for the answer. The UART
  ISR puts bytes in a lock-free ring and wakes the driver thread at each line
  end.
- Up to 5 connections (`AT+CIPMUX=1`) and passive receive
  (`AT+CIPRECVMODE=1`). Data is fetched with `AT+CIPRECVDATA` only when the
  reader has room, so a slow reader backs up into the module rather than
  losing bytes. Binary payloads are parsed by length, so data that happens to
  contain `\r\nOK\r\n` can't confuse the parser.
- Start-up: the EN pin (or `AT+RST`), `AT`, `ATE0`, `AT+GMR` (the firmware
  version is kept for the log), station mode, then DHCP or `AT+CIPSTA`.
- It retries `busy p...`, escapes `,` `"` and `\` in SSIDs, notices
  `WIFI DISCONNECT`, and recovers from a module that reboots by itself,
  joining the network again.
- Written for ESP-AT v2.x; ESP8266 AT 1.7's reply format is understood too.
- The module has one server port, so every listening client must use the
  same port. Throughput is the UART's: about 11 KB/s each way at 115200 baud.

## Using it

Ethernet, from a CubeMX project (complete in
`examples/stm32l432kc_w5500/net_app.cpp`):

```cpp
static osMutexId_t spi1Mutex = osMutexNew(nullptr);
static W5500::w5500_param_t param;
static W5500::w5500<Stm32HalSpiBlockTransport> chip(param, &hspi1, W5500_CS_GPIO_Port, W5500_CS_Pin, spi1Mutex);
static xEthernet eth(chip);

NetConfig net;
net.mac = MacAddress(0x02, 0x08, 0xDC, 0x00, 0x00, 0x01);
net.dhcp = true;
eth.begin(net);                           // before the driver thread starts
osThreadNew([](void* a) { static_cast<xEthernet*>(a)->run(); }, &eth, &netAttr);

// any other thread:
eth.waitAddress(xNetInterface::kForever);
xClient client(eth);
if (client.connect(IpAddress(192, 168, 1, 10), 80, 5000)) {
    client.write(req, reqLen, 1000);
    int32_t n = client.read(buf, sizeof buf, 2000);   // sleeps until data; -1 once closed
    client.stop();
}
```

Wi-Fi, the same way:

```cpp
static ESPAT::espat_param_t espParam;     // .hardReset: a function driving EN, optional
static ESPAT::espat<Stm32HalUartTransport> esp(espParam, &huart1);
static xWifi wifi(esp);

wifi.begin(net);                          // net.dhcp = true
osThreadNew(..., &wifi, ...);             // runs wifi.run()
wifi.join("my-ssid", "passphrase", 20000);
xClient client(wifi);                     // then exactly as above
```

RAM: each socket's stream buffers come from the FreeRTOS heap
(`Config::rxBufBytes`/`txBufBytes`, 1 KB each by default). The W5500 driver
holds a 1 KB transfer buffer; the ESP driver holds 1 KB plus a 2 KB receive
ring.

## Hardware bring-up

`examples/stm32l432kc_bringup` is a complete firmware for the NUCLEO-L432KC
that builds with CMake and the STM32CubeL4 package, with no CubeMX project.
It checks the W5500's wiring at each SPI speed and then runs at the fastest
one that passes. It checks the INT line and the PHY, then brings up DHCP
and/or the ESP module, logs each step with what to check when it fails, and
serves echo, discard and chargen. `tools/net_bringup.py` drives those
services from a PC, checking every byte and measuring latency and
throughput. See `examples/stm32l432kc_bringup/BRINGUP.md`.

## Tests

On a PC, with no hardware, HAL or RTOS:

```
cmake -S iNetTransport -B build -DSENSOR_FW_HARDWARE=HOST -DITRANSPORT_BUILD_WIRINGPI=OFF -DSENSOR_FW_BUILD_TESTS=ON
cmake --build build && ctest --test-dir build
```

- `DhcpClient_test`: the handshake, retransmission backoff, renew and
  rebind, expiry, NAK, the link coming back, and malformed or foreign
  replies, against a simulated server (`test/sim/SimDhcpServer.h`).
- `W5500_test`: the state machine on a simulated clock, against a simulated
  W5500 (`test/sim/SimW5500.h`) that decodes the real SPI frames. It covers:
  - pointer and buffer wrap, a reader that stalls, SEND_OK pacing;
  - closes from either end, listen, the interrupt path;
  - bus failure and recovery;
  - DHCP end to end, including a lease lost and replaced;
  - the bring-up probe against stuck and open wires.
- `EspAt_test`: the state machine against a simulated module
  (`test/sim/SimEspAt.h`) that answers with real ESP-AT output, including
  echo, boot noise, `busy p...`, the `>` prompt and binary payloads with
  `\r\nOK\r\n` inside. It covers start-up with and without a reset pin,
  join (escaping included), send and receive, both receive formats, closes,
  listen, an unexpected reboot, and Wi-Fi loss.
- `xNet_test`: the real FreeRTOS-layer sources on real threads, against a
  FreeRTOS simulation (`test/stub`), with both drivers. It covers blocking
  reads and their timeouts, 20 KB each way, a close waking a sleeping
  reader, connect refusal and timeout, the socket limit, listen/accept, the
  INT pin, DHCP, and `xWifi` joining and moving data through the ESP driver.
- `spi_block_transport_test` (in itransport): the two SPI phases under one
  chip-select, chained from the interrupt, with one wake-up per transfer.
