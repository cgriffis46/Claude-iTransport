# iNetTransport

Network interfaces on top of itransport: `xEthernet`, `xWifi` and `xClient`
for FreeRTOS, with two chip drivers underneath. One is the WIZnet W5500
(Ethernet over SPI). The other is an Espressif module running ESP-AT
firmware (Wi-Fi over a UART). There are also DHCP, DNS and SNTP clients,
so an interface gets its address, looks up names and keeps the time, and
an MQTT client (`xMqttClient`) and a web server (`xHttpServer`) that run
over either interface.
Target: STM32L432KC, with one interface (the W5500 or an ESP module, not
both), most likely as a node that sends its data out: an MQTT or HTTP
client. The servers (`xHttpServer`) fit it with `maxClients = 1`, but are
meant for bigger STM32s.

## Layers

```
 user threads   xMqttClient  publish / subscribe / receive    (mqtt/: MqttClient)
                   |       its own thread keeps the session, over an xClient
                xHttpServer  GET / POST handlers              (http/: lexer, state machine)
                   |       a daemon thread, and a thread per client, over xClients
                xClient  connect / listen / accept / read / write / stop
                   |       (each sleeps on its socket's event group, with a timeout)
 hw/freertos    xEthernet / xWifi  ->  xNetInterface
                   |       driver thread; FreeRTOS queue (inbox),
                   |       per-socket stream buffers + event group
 inc/           iNetDevice / iEthernetDevice / iWifiDevice
                   |       the seam: any chip, any bus
 w5500/         w5500<TTransport>        espat/   espat<TTransport>
                   |   + dhcp/ dns/ sntp/            |   (the module does DHCP,
                   |     on one UDP socket           |    DNS and SNTP itself)
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
  packets out, lease timers. The W5500 driver runs it on socket 7, its UDP
  service socket (`w5500_param_t::serviceSocket`, on by default). The
  interface then gets sockets 0 to 6. It covers
  DISCOVER/OFFER/REQUEST/ACK with backoff, renewing at T1 (unicast) and
  rebinding at T2 (broadcast), expiry, NAK, and a fresh check when the
  cable comes back. The lease includes the DNS server and, when the network
  offers one, an NTP server (option 42).
- **ESP-AT**: the module does DHCP itself. The driver reads the result back
  with `AT+CIPSTA?` and `AT+CIPDNS?`.

## Names and time

```cpp
IpAddress ip;
if (eth.resolve("api.example.com", ip, 5000)) { ... }   // sleeps for the answer
uint64_t now = eth.unixTimeMs();                         // UTC ms; 0 until the time is known
```

- `resolve(host, ip, timeoutMs)` looks up an IPv4 address with the
  interface's DNS server (from DHCP, or `NetConfig::dns`). An `"a.b.c.d"`
  string is answered at once. Threads that call it together queue, and
  each gets its own answer. An answer that arrives after its caller gave
  up is never handed to the next caller.
- The time is kept without being asked. As soon as there is an address,
  the interface asks a time server, and asks again every
  `Config::ntpIntervalMs` (an hour by default). The server is the one DHCP
  named, else `Config::ntpServer` (`pool.ntp.org`). `syncTime(timeoutMs)`
  asks now; `timeValid()` and `unixTimeMs()` read the time. It runs on
  the RTOS tick between syncs.
- **W5500**: `dns/DnsClient` and `sntp/SntpClient` are pure logic, like
  DHCP, run by the driver on the same UDP service socket (port 68). Replies
  are told apart by the port they come from (67, 53, 123) and checked
  against their query:
  - DNS: the query ID and the question; CNAMEs are followed.
  - SNTP: a random nonce echoed back; kiss-o'-death and unsynchronised
    servers are rejected; the round trip is halved. Accuracy is a few ms on
    a LAN.
- **ESP-AT**: the module's own `AT+CIPDOMAIN` (names up to 64 characters)
  and SNTP (`AT+CIPSNTPCFG`, then `AT+CIPSNTPTIME?` until it has
  synchronised). The module reports whole seconds, so this time is good to
  about half a second.

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

## MQTT

```cpp
xMqttClient::Config cfg;
cfg.host = "broker.local";                  // a name or "a.b.c.d"; port 1883
cfg.session.clientId = "node-1";            // also username, password, keepAliveSec, will, cleanSession
static xMqttClient mqtt(eth, cfg);          // or wifi
mqtt.begin();
osThreadNew([](void* m) { static_cast<xMqttClient*>(m)->run(); }, &mqtt, &mqttAttr);

// any other thread:
mqtt.subscribe("nodes/node-1/cmd/#", 1);    // true once granted; kept across reconnects
mqtt.publish("nodes/node-1/temp", "21.5");  // QoS 0
mqtt.publish("nodes/node-1/alarm", "hi", 1, false, 2000);   // QoS 1: true once acknowledged
uint8_t buf[1100];                          // Config::maxPacket + 4
MqttMessage m;
if (mqtt.receive(buf, sizeof buf, m, 5000)) { /* m.topic, m.payload, m.len */ }   // sleeps until one comes
```

- MQTT 3.1.1, QoS 0 and 1, no TLS. `mqtt/MqttClient` is the protocol as
  pure logic, like DHCP: packets in, packets out, the session's timers.
  It allocates nothing; the buffers are the caller's.
- `xMqttClient::run()` is the MQTT thread. It waits for an address, looks
  the broker up, connects, and reconnects with backoff
  (`reconnectMinMs`, doubling up to `reconnectMaxMs`). It sends PINGREQ
  every keepalive while idle and drops the connection when the broker
  stops answering (no PINGRESP within half the keepalive, at least 3 s)
  or doesn't send CONNACK within 10 s. `stop()` ends with a DISCONNECT,
  so the broker doesn't publish the will.
- After a reconnect, subscriptions are sent again in one SUBSCRIBE (unless
  the broker kept the session). QoS 1 messages not yet acknowledged are
  sent again with DUP. A QoS 1 publish while disconnected is kept and sent
  once connected; up to `MqttClient::kMaxInFlight` (4) at a time.
- Publishes go out from the calling thread, under the session's mutex, so
  they don't wait for the MQTT thread to wake. Received messages go into
  a FreeRTOS message buffer (`inboxBytes`) for `receive()`. With
  `setCallback()` they go to a function on the MQTT thread instead, which
  may publish. A message that doesn't fit is dropped rather than waited
  for. If it is QoS 1 it isn't acknowledged, so the broker sends it again
  after the next reconnect. `stats()` counts these.
- RAM: `maxPacket` (1 KB) twice plus a framing buffer of the same size,
  `inFlightBytes` (2 KB) for kept QoS 1 messages, and `inboxBytes` (2 KB),
  all from the FreeRTOS heap, plus one socket's stream buffers. Up to 8
  subscriptions of up to 64 characters are remembered. On the target the
  code is about 6 KB (`-Os`, Cortex-M4).

## Web server

```cpp
static void temp(const HttpRequest& req, HttpResponse& res, void*) {
    res.begin("application/json");
    res.printf("{\"t\":%.1f}", readTemp());
}
static void led(const HttpRequest& req, HttpResponse& res, void*) {
    char state[8];
    if (!req.param("state", state, sizeof state)) { res.status(422).send("text/plain", "state?"); return; }
    setLed(std::strcmp(state, "on") == 0);
    res.send("text/plain", "ok");
}

static xHttpServer web(eth, xHttpServer::Config());   // port 80, 2 clients
web.get("/temp", temp);
web.post("/led", led);            // form fields or ?state=on
web.get("/api/*", api);           // a prefix
web.begin();
osThreadNew([](void* w) { static_cast<xHttpServer*>(w)->run(); }, &web, &webAttr);
```

How a request flows:

```
 socket bytes --> HttpLexer --> FreeRTOS queue --> HttpConnection --> handler
   (xClient)      a byte at a    of HttpTokens      state machine:     (HttpRequest,
                  time: method,  (fixed size;       assembles the      HttpResponse)
                  target, headers, long text in     request, routes,
                  body by length  pieces)           answers errors
```

- **Threads.** `run()` is the daemon. It listens on the port, accepts,
  and creates a thread (`xTaskCreate`) for each client. That thread
  reads, lexes, parses and runs the handlers, then deletes itself when
  the connection ends. A handler can block, and only its own client
  waits. Up to `maxClients` at once (each holds a socket). A W5500 socket
  listens for one connection at a time. While every client slot is busy,
  or for the moment between accepting and listening again, new
  connections are refused (TCP RST), and a browser retries.
- **Tokens.** `strtok()` won't do for a socket: it needs the whole text
  in one NUL-terminated buffer and writes into it, while a request
  arrives in pieces split anywhere. `HttpLexer` is a state machine over
  the request grammar instead, fed whatever has arrived. It checks the
  syntax, trims header values, and frames the body by Content-Length.
  Tokens carry up to 40 bytes; longer text comes as several. When the
  queue fills, the lexer stops, the state machine empties the queue, and
  the lexer carries on where it was.
- **Requests.** HTTP/1.0 and 1.1, keep-alive and pipelining. The method,
  path (percent-decoded), query, headers and body go into one buffer per
  client (`requestBytes`). `req.param()` reads the query string and
  `application/x-www-form-urlencoded` bodies. `Expect: 100-continue` is
  answered. Request bodies need a Content-Length (chunked uploads get
  501).
- **Responses.** `send()` with a length, or `begin()` then
  `write()`/`print()`/`printf()` to stream. Streamed responses are sent
  chunked to HTTP/1.1 clients, and end with the connection for HTTP/1.0
  ones. HEAD goes to the GET handler, without the body. A handler that
  answers nothing gets a 500.
- **Answered by the server:** 400 (malformed, or HTTP/1.1 without Host),
  404, 405 (with Allow), 408 (`requestTimeoutMs` from a request's first
  byte), 413 (body over `requestBytes`, before it is read), 414, 417, 431,
  501, 503 (no thread could be created) and 505. Every error closes the
  connection except 404 and 405. Idle kept connections close after
  `idleTimeoutMs`.
- **RAM per client:** `requestBytes` (2 KB) plus the token queue (8 tokens,
  about 400 B) and about 600 B of state, allocated in `begin()`. A client
  thread's stack (`stackWords`, 3 KB) only exists while that client is
  connected. On the target the code is about 8 KB (`-Os`, Cortex-M4),
  plus newlib's `vsnprintf` if `printf()` is used. No TLS, no files.

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

Then, from any thread, on either interface: `resolve()` for names and
`unixTimeMs()` for the time (see Names and time).

RAM: each socket's stream buffers come from the FreeRTOS heap
(`Config::rxBufBytes`/`txBufBytes`, 1 KB each by default). The W5500 driver
holds a 1 KB transfer buffer; the ESP driver holds 1 KB plus a 2 KB receive
ring.

## Hardware bring-up

`examples/stm32l432kc_bringup` is a complete firmware for the NUCLEO-L432KC
that builds with CMake and the STM32CubeL4 package, with no CubeMX project.
It checks the W5500's wiring at each SPI speed and then runs at the fastest
one that passes. It checks the INT line and the PHY, then brings up DHCP
or the ESP module (one per build), looks up a name and sets the time. It logs each step
with what to check when it fails, and serves echo, discard, chargen and
time. `tools/net_bringup.py` drives those services from a PC, checking
every byte, measuring latency and throughput, and comparing the board's
clock with the PC's. See `examples/stm32l432kc_bringup/BRINGUP.md`.

## Tests

On a PC, with no hardware, HAL or RTOS:

```
cmake -S iNetTransport -B build -DSENSOR_FW_HARDWARE=HOST -DITRANSPORT_BUILD_WIRINGPI=OFF -DSENSOR_FW_BUILD_TESTS=ON
cmake --build build && ctest --test-dir build
```

- `DhcpClient_test`: the handshake, retransmission backoff, renew and
  rebind, expiry, NAK, the link coming back, option 42, and malformed or
  foreign replies, against a simulated server (`test/sim/SimDhcpServer.h`).
- `DnsClient_test`: the query's bytes, CNAMEs through compressed names,
  NXDOMAIN and SERVFAIL, retries and timeout, bad names, and replies with
  the wrong ID, the wrong question or cut short (`test/sim/SimDnsServer.h`).
- `SntpClient_test`: the request, the time to the ms with the round trip
  halved, the 2036 rollover, kiss-o'-death, an unsynchronised server, a
  forged or late reply, and retries (`test/sim/SimNtpServer.h`).
- `W5500_test`: the state machine on a simulated clock, against a simulated
  W5500 (`test/sim/SimW5500.h`) that decodes the real SPI frames. It covers:
  - pointer and buffer wrap, a reader that stalls, SEND_OK pacing;
  - closes from either end, listen, the interrupt path;
  - bus failure and recovery;
  - DHCP end to end, including a lease lost and replaced;
  - DNS and SNTP on the service socket: a server named or looked up first,
    requests held until there is an address, a lookup replaced, the chip
    failing mid-lookup;
  - the bring-up probe against stuck and open wires.
- `EspAt_test`: the state machine against a simulated module
  (`test/sim/SimEspAt.h`) that answers with real ESP-AT output, including
  echo, boot noise, `busy p...`, the `>` prompt and binary payloads with
  `\r\nOK\r\n` inside. It covers start-up with and without a reset pin,
  join (escaping included), send and receive, both receive formats, closes,
  listen, `AT+CIPDOMAIN`, the module's SNTP, an unexpected reboot, and
  Wi-Fi loss.
- `MqttClient_test`: CONNECT's bytes (credentials, will, keepalive),
  refusal and a missing CONNACK, QoS 0 and 1 publishes and PUBACK, store and
  forward with DUP on resend, subscribe and receive (with PUBACK for QoS 1),
  re-subscribing after a reconnect, keepalive and an unanswered ping,
  malformed and oversized packets, and topic matching, against a simulated
  broker (`test/sim/SimMqttBroker.h`) with its own decoder.
- `xMqtt_test`: `xMqttClient` on real threads over the W5500 driver, with
  the simulated chip handing the TCP connection to the simulated broker.
  It covers looking the broker up, a `receive()` sleeping until a message
  comes, a QoS 1 publish waiting for its ack, a dropped connection
  (reconnect, re-subscribe, DUP resend), a refused connection with backoff
  and store and forward, keepalive, callback mode (publishing from the
  callback), a full inbox, and four threads publishing at once.
- `Http_test`: the lexer whole, a byte at a time and in random pieces,
  through a queue of one; long tokens in pieces; malformed requests (each
  with its status); then requests end to end. That covers routing,
  prefixes, HEAD, 404/405, forms and queries, chunked streaming,
  pipelining, HTTP/1.0 and Connection, `maxRequests`, every error status,
  `100-continue`, absolute-form targets, timeouts and a failing output.
- `xHttp_test`: `xHttpServer` on real threads over the W5500 driver, with
  simulated browsers on the chip's far end. It covers a thread created per
  connection, pipelining, a form arriving slowly in pieces, a 14 KB
  streamed page, 413, three clients at once (slow handlers in parallel
  without holding up a fast one, nothing listening while all are busy),
  a closed connection freeing its slot, idle and request timeouts, a
  thread that can't be created (503), and `stop()` ending every thread.
- `xNet_test`: the real FreeRTOS-layer sources on real threads, against a
  FreeRTOS simulation (`test/stub`), with both drivers. It covers blocking
  reads and their timeouts, 20 KB each way, a close waking a sleeping
  reader, connect refusal and timeout, the socket limit, listen/accept, the
  INT pin, DHCP, and `xWifi` joining and moving data through the ESP driver.
  It also covers `resolve()` from two threads at once and after a timeout,
  the time set by itself and by `syncTime()`, and DHCP's NTP server.
- `spi_block_transport_test` (in itransport): the two SPI phases under one
  chip-select, chained from the interrupt, with one wake-up per transfer.
