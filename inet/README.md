# inet

Network interfaces on top of itransport: `xEthernet`, `xWifi` and `xClient`
for FreeRTOS, and a non-blocking WIZnet W5500 driver underneath. Target:
STM32L432KC.

## Layers

```
 user threads        xClient  connect / listen / accept / read / write / stop
                        |       (each sleeps on its socket's event group, with a timeout)
 hw/freertos         xEthernet / xWifi  ->  xNetInterface
                        |       driver thread; FreeRTOS queue (inbox),
                        |       per-socket stream buffers + event group
 inc/                iNetDevice / iEthernetDevice / iWifiDevice
                        |       the seam: any chip, any bus
 w5500/              w5500<TTransport>   non-blocking state machine, poll()
                        |
 itransport          iBlockTransport -> SpiBlockTransport -> Stm32HalSpiBlockTransport
                                         (header then data, one chip-select; DMA)
```

The bus is the chip driver's business, not the interface's. A chip on SPI
uses `iBlockTransport` (W5500, ATWINC1500), a module on a UART uses the
existing stream `iTransport` (ESP-AT Wi-Fi), and a PHY on MII/RMII would sit
behind the MCU's Ethernet MAC and a host stack such as lwIP. All of them
implement `iNetDevice`, and `xEthernet`/`xWifi`/`xClient` don't change. (The
STM32L432 has no Ethernet MAC, so MII/RMII doesn't apply to this target.)

## Threads and blocking

- **One driver thread** owns the chip driver and is the only thread that
  touches it. It runs `eth.run()`: it handles requests from its inbox (a
  FreeRTOS queue), calls the device's `poll()`, then sleeps on the inbox for
  as long as `poll()` says it can. A request, a write (as a "kick") or the
  chip's INT pin (`interruptFromIsr()`) wakes it at once.
- **Data** goes through two stream buffers per socket: the driver thread
  writes `rx` and reads `tx`, and the socket's user thread does the reverse.
  That is one writer and one reader each, so no locks are needed.
- **Sleeping**: `client.read(buf, len, timeoutMs)` sleeps on the socket's
  event group until bytes arrive, the connection closes, or the timeout runs
  out. The event group also covers what a stream buffer can't signal, such as
  the peer closing while a reader sleeps. `write`, `connect`, `accept` and
  `flush` sleep the same way. Nothing polls.
- **Flow control**: when a reader falls behind, the driver stops taking
  bytes off the W5500, so the chip's TCP window closes. The next `read()`
  that makes room wakes the driver.

## W5500 driver

`w5500<TTransport>` works like the sensor drivers. Each register access is
one state that issues the transfer and one that waits for it to land, and
the chip is never waited on. It doesn't call `osDelay()`. Instead `poll()`
returns how long it can be left, so the driver thread stays responsive.

- Start-up: soft reset, VERSIONR check, MAC/IP/gateway/subnet in one burst,
  RTR/RCR, per-socket buffer sizes, socket interrupts on, and the PHY link.
- Sockets: TCP client (`connect`) and server (`listen`; each socket is one
  connection). Graceful close, with a forced CLOSE after a timeout.
- Data: Sn_RX_RSR/Sn_TX_FSR are read until two reads agree, as the datasheet
  says. Up to 1 KB moves per SPI transfer, several transfers are written
  before one SEND, and only one SEND is in flight at a time.
- Interrupt or polling: with INT wired, SIR is read when the pin fires and
  polling is only a backstop (`pollMs`). Without it, SIR is polled every
  `pollMs` (5 ms by default).
- Errors: a failed or timed-out transfer, or a chip that doesn't answer,
  reports every socket as Failed, backs off and starts again from reset.
- Not yet: UDP, DHCP and DNS. Addresses are static.

## Using it

See `examples/stm32l432kc_w5500/net_app.cpp` for a complete TCP echo server
with its CubeMX settings (SPI1 with DMA, CS, and INT on EXTI). In short:

```cpp
static osMutexId_t spi1Mutex = osMutexNew(nullptr);
static W5500::w5500_param_t param;
static W5500::w5500<Stm32HalSpiBlockTransport> chip(param, &hspi1, W5500_CS_GPIO_Port, W5500_CS_Pin, spi1Mutex);
static xEthernet eth(chip);

eth.begin(netConfig);                     // before the driver thread starts
osThreadNew([](void* a) { static_cast<xEthernet*>(a)->run(); }, &eth, &netAttr);

// any other thread:
xClient client(eth);
if (client.connect(IpAddress(192, 168, 1, 10), 80, 5000)) {
    client.write(req, reqLen, 1000);
    int32_t n = client.read(buf, sizeof buf, 2000);   // sleeps until data, -1 once closed
    client.stop();
}
```

RAM: each socket's stream buffers come from the FreeRTOS heap
(`Config::rxBufBytes`/`txBufBytes`, 1 KB each by default). The W5500 driver
object holds a 1 KB transfer buffer.

## Tests

On a PC, with no hardware, HAL or RTOS:

```
cmake -S inet -B build -DSENSOR_FW_HARDWARE=HOST -DITRANSPORT_BUILD_WIRINGPI=OFF -DSENSOR_FW_BUILD_TESTS=ON
cmake --build build && ctest --test-dir build
```

- `W5500_test`: the state machine on a simulated clock, against a simulated
  W5500 (`test/sim`) that decodes the real SPI frames. Covers pointer and
  buffer wrap, the reader stalling, SEND_OK pacing, peer and local close,
  listen, the interrupt path, bus failure and recovery.
- `xNet_test`: the real FreeRTOS-layer sources on real threads, against a
  FreeRTOS simulation (`test/stub`). A user thread sleeps in `read()` while
  the driver thread runs. Covers blocking reads and their timeouts, 20 KB
  each way through every buffer, a close waking a sleeping reader, connect
  refusal and timeout, the socket limit, listen/accept, the INT pin, and
  `xWifi` joining with a fake module.
- `spi_block_transport_test` (in itransport): the two SPI phases under one
  chip-select, chained from the interrupt, with one wake-up per transfer.
