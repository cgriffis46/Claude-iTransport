# iNetTransport

Network interfaces on top of itransport: `xEthernet`, `xWifi` and `xClient`
for FreeRTOS, with four drivers underneath. One is the WIZnet W5500
(Ethernet over SPI). One is an Espressif module running ESP-AT firmware
(Wi-Fi over a UART). One is Microchip's ATWINC1500 (Wi-Fi over SPI). The
fourth, `SocketNetDevice`, runs on a TCP/IP stack's
socket API: lwIP's on an MCU with its own Ethernet MAC (the STM32F207) or
on an ESP32, or the operating system's on Linux. There are also DHCP, DNS and SNTP clients,
so an interface gets its address, looks up names and keeps the time, and
an MQTT client (`xMqttClient`), an HTTP client (`xHttpClient`) and a web
server (`xHttpServer`) that run over either interface.
Target: STM32L432KC, with one interface (the W5500, an ESP module or an
ATWINC1500, never two), most likely as a node that sends its data out: an MQTT or HTTP
client. The servers (`xHttpServer`) fit it with `maxClients = 1`, but are
meant for bigger STM32s, such as the F207 with lwIP, where the same code
runs on `SocketNetDevice`.

## Layers

```
 user threads   xMqttClient  publish / subscribe / receive    (mqtt/: MqttClient)
                   |       its own thread keeps the session, over an xClient
                xHttpClient  get / post / request             (http/: lexer, response reader)
                   |       on the caller's thread, over an xClient
                xHttpServer  GET / POST handlers              (http/: lexer, state machine)
                   |       a daemon thread, and a thread per client, over xClients
                xClient  connect / listen / accept / read / write / stop
                   |       (each sleeps on its socket's event group, with a timeout)
 hw/freertos    xEthernet / xWifi  ->  xNetInterface
                   |       driver thread; FreeRTOS queue (inbox),
                   |       per-socket stream buffers + event group
 inc/           iNetDevice / iEthernetDevice / iWifiDevice
                   |       the seam: any chip, any bus
 w5500/         w5500<TTransport>        espat/   espat<TTransport>    sockets/  SocketNetDevice
                   |   + dhcp/ dns/ sntp/            |   (the module does         |  + dns/ sntp/ on a
                   |     on one UDP socket           |    DHCP, DNS, SNTP)        |    UDP socket
 itransport     iBlockTransport (SPI, DMA)          iTransport (UART stream)   lwIP sockets (MAC + PHY)
                                                                               or the OS's
```

The bus is the chip driver's business, not the interface's. A chip on SPI
uses `iBlockTransport` (W5500, ATWINC1500). A module on a UART uses the
stream `iTransport` (ESP-AT). A PHY on MII/RMII would sit behind the MCU's
Ethernet MAC and a host stack such as lwIP: that's `SocketNetDevice`. All
of them implement `iNetDevice`, so `xEthernet`, `xWifi` and `xClient`, and
everything on them, don't change. The STM32L432 has no Ethernet MAC, so it
uses the W5500 or an ESP module; the F207 has one, and uses lwIP.

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

## Socket driver: lwIP, or the operating system

`SocketNetDevice` is an `iEthernetDevice` on a BSD socket API, so a board
whose TCP/IP stack is lwIP runs the same interfaces, web server and clients
as one with a W5500:

```cpp
// STM32F207, CubeMX with LwIP and FreeRTOS: MX_LWIP_Init() has set up the
// MAC, the PHY and DHCP. Build iNetTransport with INET_SOCKETS_LWIP.
SocketNetDevice::Config devCfg;
devCfg.platform = lwipNetifPlatform(&gnetif);   // link and address from lwIP's netif
static SocketNetDevice dev(devCfg);
static xEthernet eth(dev);
eth.begin(net);                                 // net.dns: used if lwIP's DNS has none
osThreadNew([](void* e) { static_cast<xEthernet*>(e)->run(); }, &eth, &netAttr);
// From here on, exactly as with the W5500: xClient, xHttpServer, xHttpClient, xMqttClient.
```

- lwIP owns the interface: MAC, PHY, address, DHCP. The device only reads
  the link and the address through `Platform` (`hw/lwip/LwipNetif`), and
  tells the interface when they change. Every socket goes when the
  address does.
- Needs `LWIP_SOCKET 1` (so `NO_SYS 0`). Any `LWIP_COMPAT_SOCKETS` works:
  `NetSockets.h` undoes the compat macros (`connect`, `poll`, `close`,
  ...) that would otherwise rewrite this project's methods of those names.
  In your own files, include lwIP's socket header after this project's
  headers, or set `LWIP_COMPAT_SOCKETS 2`.
- Non-blocking: `poll()` does one zero-timeout `select()` over every
  socket. It asks to be called again within 2 ms while a socket is open,
  and within 50 ms otherwise. Received data waits at most one interval.
- Several sockets listening on one port share one listening socket.
  Connections wait in its backlog rather than being refused between
  accepts, which is better than the W5500 can do.
- DNS and SNTP use the same `DnsClient` and `SntpClient` as the W5500, on
  a UDP socket of the device's own, so lwIP's own DNS isn't needed.
- About 4.7 KB of code and 4 KB of RAM for 8 sockets on a Cortex-M3,
  plus lwIP itself. lwIP's sockets cost more RAM than the raw API that
  `CipTagTcpServer` uses; the F207's 128 KB has room for both.
- On Linux, the same source (without `INET_SOCKETS_LWIP`) runs on the
  operating system's sockets. That is how the tests drive it with curl.

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

## ATWINC1500 driver

`winc1500<TTransport>` (`winc1500/`) drives Microchip's (Atmel's) ATWINC1500
Wi-Fi module (Adafruit's breakout, the Feather M0 WiFi's module, Arduino's
MKR1000) over any `iBlockTransport` (`Stm32HalSpiBlockTransport` on the L432).
It was written here, non-blocking, with Microchip's host driver 19.5.2 (BSD-3,
as shipped in Arduino's WiFi101 library) as the reference for a protocol that
has no other public description.

```cpp
WINC1500::winc_param_t p;
p.hardReset = [](bool asserted) { HAL_GPIO_WritePin(WINC_RST_GPIO_Port, WINC_RST_Pin,
                                                    asserted ? GPIO_PIN_RESET : GPIO_PIN_SET); };
p.pollMs = 100;                       // IRQN wired to interruptFromIsr()
static WINC1500::winc1500<Stm32HalSpiBlockTransport> winc(p, &hspi1, WINC_CS_GPIO_Port, WINC_CS_Pin, spiMutex);
static xWifi wifi(winc, cfg);         // cfg.rxBufBytes = 1400 or more: see below
// in HAL_GPIO_EXTI_Callback: if (pin == WINC_IRQN_Pin) wifi.interruptFromIsr();
```

- **Wiring.** SCK, MISO, MOSI, CS (SPI mode 0, up to 48 MHz; 10 MHz in the
  bring-up firmware), IRQN (active low: a falling-edge interrupt with a
  pull-up), RESET_N (`hardReset`); CHIP_EN and WAKE tied to 3.3 V. Without a
  reset pin it resets the module through a register. The module draws about
  300 mA peaks.
- **Start-up.** A reset, then the SPI with CRC-7 on its commands, which the
  driver turns off, as Microchip's does. Then the chip ID, the boot ROM, the
  host's version (it says 19.5.2), the firmware's "init done", IRQN on, and
  the firmware's version and MAC from its memory. `error()` says why a start
  failed: no answer, another chip, firmware older than 19.5.0
  (`firmware_too_old`: update it with Arduino's or Microchip's updater), or
  one needing a newer host driver. It retries every second by itself.
- **Every access** is a few SPI transfers (the command, its echo and state
  read a byte at a time, the data), each one state of `poll()`, never a wait.
  A failed access is repeated after an SPI reset command, 10 times, as
  Microchip's driver does.
- **Messages.** Requests go into a buffer the module hands out (HIF); the
  module's messages wait in its memory until the driver says "RX done". A
  module that hands out no buffer for 5 s is restarted (it may have reset
  behind our back).
- **Wi-Fi.** `join()` with WPA2 (8 to 63 characters, or 64 hex digits) or
  an open network (empty passphrase); the passphrase isn't saved in the
  module's flash. `joinError()`: 1 no such network, 3 wrong passphrase. The
  module does DHCP, or takes a static address. RSSI is asked for every
  `rssiPollMs`. The module doesn't rejoin by itself; the driver does, every 5
  s, when the network goes away, until `leave()` or another `join()`.
- **Sockets.** 7 TCP sockets on the module; the interface gets `sockets` (6)
  of them. A listen takes one module socket per port, shared by every
  interface socket listening on it, and each accepted connection another. A
  peer arriving while nobody listens is closed; a listening socket nobody
  wants closes after 2 s. Two ports at once.
- **Data.** One send of up to 1400 bytes in flight per socket. One receive
  outstanding per socket: a reply (up to 1400 bytes) is copied into the
  driver's buffer and the module released at once, then handed to the
  reader as it has room. A reader that falls behind by more than its buffer
  keeps the driver's buffer, and replies for other sockets wait in the
  module until it catches up, so give sockets read slowly
  `rxBufBytes` of 1400 or more.
- **DNS and time** are the module's: `resolve()` asks it, `requestTime()`
  reads its clock (the firmware's SNTP client, its own servers; to the
  second) and asks again each second until it is set, up to 15 s.
- **Size** (L432, `-Os`): 11.6 KB of code, 3.9 KB of RAM (a 1.5 KB message
  buffer and a 1.4 KB receive buffer among it).
- **Not here:** TLS on the module, UDP, power save, access point and
  provisioning modes, scanning, WPS, 802.1X and WEP networks, setting the
  SNTP server, the module's OTA update.
- **Checked against Microchip's driver.** `winc1500/test/ref/winc_numbers.c`
  printed every opcode, register, size and offset from Microchip's headers
  for the ARM ABI; `WincRef_test` (`-DWINC_REF_DIR=<WiFi101>`) static_asserts
  `WincProtocol.h` against those headers, and runs Microchip's driver
  itself on the PC against the simulated module the driver's tests use:
  start-up, join, DHCP, RSSI, time, TCP both ways, a listen, DNS, with the
  simulator finding nothing it didn't expect. Nothing has run on a module.

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

## HTTP client

For a node sending its readings to a server:

```cpp
static xHttpClient http(eth);             // or wifi

char json[64];
int n = std::snprintf(json, sizeof json, "{\"node\":3,\"t\":%.1f}", t);
uint8_t reply[128];                       // the response's body, NUL-terminated if it fits
xHttpClient::Response r;
uint16_t status = http.post("http://sensors.local:8080/api/readings", "application/json",
                            json, n, reply, sizeof reply, r, 5000);
if (status == 0) { /* http.error(): Resolve, Connect, Timeout, Closed, ... */ }
```

- Every call runs on the calling thread and sleeps up to its timeout. The
  client has no thread of its own and allocates nothing: it is 788 bytes
  on a Cortex-M4, plus the socket's buffers while connected, and about
  5 KB of code. One thread per client, as for `xClient`.
- `get()`, `post()`, or `request()` for any method, extra headers
  (`"X-Key: abc\r\n"`), a header callback, and `onBody` to stream a body
  of any size instead of buffering it. A body bigger than the buffer keeps
  its start, and `Response::truncated` is set.
- The host is a name (looked up with the interface's DNS) or `a.b.c.d`,
  with an optional port. No TLS: `https://` fails with `Unsupported`.
- The response goes through `HttpLexer` in Response mode, the same token
  stream as the server's, straight into `HttpResponseReader`. The client
  reads and parses on one thread, so there is no queue between them. It
  handles Content-Length, chunked (decoded), bodies that run to the close,
  `100 Continue`, and HEAD/204/304 without a body.
- The connection is kept for the next request to the same host and port,
  unless either side says close. A server may drop a kept connection
  while it's idle. That is noticed before sending. If it happens just as a
  request goes out, a GET, HEAD, PUT, DELETE or OPTIONS is sent again on
  a new connection. A POST isn't, since it may have been acted on: it
  fails with `Closed`, and the caller decides.

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
  plus newlib's `vsnprintf` if `printf()` is used. HTTPS: see "HTTPS and
  logins"; files: see "The web UI from files".

### The web UI from files

The pages don't have to be in the firmware. `HttpStaticFiles` serves them
from an `HttpFileSource`, so the UI can change without reflashing:

```cpp
static HttpStdioFiles disk("0:/www");        // FILE*: Linux, or newlib reaching FatFs
static HttpMemoryFiles builtIn(files, n);   // arrays in flash: a fixed UI, or the fallback
static HttpStaticFiles site(disk, &builtIn); // up to three sources, tried in order
web.get("/api/tags", ...);                   // API routes first
web.get("/*", HttpStaticFiles::handler, &site);
```

- `/` and any path ending in `/` serve `index.html`. The type comes from
  the extension. Files are sent with `Cache-Control: no-cache`, so a
  changed UI shows on the next load.
- When the browser takes gzip and `<file>.gz` exists, that is sent
  (`Content-Encoding: gzip`). A UI can be stored compressed, which is
  handy on small flash.
- A file missing from the first source comes from the fallback (the
  built-in page), so a blank card still gives a working UI.
- Paths with a `..` segment or a backslash get 404. Nothing outside the
  folder is reachable, encoded or not. Neither are hidden files (any
  part of the path starting with `.`), which is where uploads in
  progress are kept.
- On an STM32, use `storage/` below: `HttpLittleFsFiles` (SPI flash) or
  `HttpFatFsFiles` (an SD card) are sources written straight on the file
  system. `HttpStdioFiles` also works if newlib's `_open`/`_read`/`_lseek`
  are wired to one.

### Storage: SPI flash and SD cards

`storage/` keeps the UI's files where they can be replaced from a
browser: LittleFS on a SPI NOR flash chip, or FatFs on an SD card. Both
serve files (`HttpFileSource`) and take uploads (`HttpFileStore`), and
`HttpFileAdmin` puts the uploads behind an admin login.

```cpp
// SPI flash (W25Q, MX25L, IS25LP, SST26...) on an iBlockTransport,
// e.g. Stm32HalSpiBlockTransport with the chip's own CS pin.
static SpiNorFlash::Config f;           // yield: osDelay(1) or taskYIELD; now: a ms clock
static SpiNorFlash flash(spiBlock, f);
static TlsFreeRtosLock fsLock;          // any iLock
static LittleFsNor::Config l;
l.lock = &fsLock;
l.firstSector = 256;                    // keep the first 1 MB for something else
static LittleFsNor fs(flash, l);
flash.begin();  fs.mount();             // a blank chip is formatted
static HttpLittleFsFiles files(fs, "/www");

// or an SD card: CubeMX's FATFS middleware (FF_USE_LFN 2 or 3), mounted
// with f_mount(&sd, "0:", 1) as usual.
static HttpFatFsFiles files("0:/www", &fsLock);

static HttpMemoryFiles builtIn(firmwareFiles, n);   // with httpFileAdminPage in it
static HttpStaticFiles site(files, &builtIn);       // the storage first, then the firmware's
HttpFileAdmin::Config a;
a.auth = &auth;                                     // WebAuth, see "HTTPS and logins"
a.sleep = [](uint32_t ms, void*) { osDelay(ms); };  // lets a commit wait for its readers
static HttpFileAdmin admin(files, a);
admin.attach(web);                                  // before the "/*" route
web.get("/*", HttpStaticFiles::handler, &site);
```

- **Uploads.** `/files.html` (`httpFileAdminPage`, in the firmware) logs
  in, lists the files with the space left, uploads and deletes. A file
  goes up in pieces of `pieceBytes` (1 KB), so each request fits the
  server's `requestBytes`: `PUT /api/files/<path>?offset=N`, then
  `POST /api/files/<path>?size=N`. The pieces go into a hidden
  `.name.part` beside the file, and the commit puts it in place, so the
  old file is served until the new one is complete. Every call needs an
  admin session (`Config::role`), changes need the CSRF token, and each
  upload, commit and delete goes to `Config::audit`. Files are limited to
  `maxFileBytes` (512 KB).
- **Replacing a file someone is downloading.** A file open for reading
  keeps its blocks only while it has a name, so neither file system may
  replace or delete it under a reader. LittleFS's allocator doesn't
  protect files that are only being read, and FatFs with `FF_FS_LOCK 0`
  doesn't either. A test with slow readers got garbage in 29 of 43 reads
  before this was fixed. So `commit()` and `remove()` refuse a file that
  is open (`busy()`). `HttpFileAdmin` then holds it closed to new readers
  (they get the next source's copy, such as the built-in page), waits for
  the readers it has, and commits, all within `busyWaitMs` (5 s), given
  `Config::sleep`. Without that, or when time runs out, it answers 503
  with `Retry-After: 1` and the page tries again for 30 s.
- **Built in first.** With the firmware's files as the second source, a
  blank chip or card still serves the built-in UI and `/files.html`.
  Upload an `index.html` and it takes over; delete it and the built-in
  one is back.
- **Power loss.** LittleFS is copy-on-write: replacing a file is atomic.
  The test cuts the power at every write of an update, and the old or
  the new file survives whole each time. FatFs can't rename over a file,
  so its commit deletes the old file first. A power cut in between
  leaves only the `.part`, and the built-in page is served until the
  upload is repeated.
- **Threads.** Everything goes through one `iLock` (`TlsFreeRtosLock` is
  one, and `WebAuth::Lock` and `MbedTlsServer::Lock` are now the same
  type). `HttpLittleFsFiles` and `HttpFatFsFiles` keep 4 files open for
  reading (`kMaxOpen`), one per client thread, and one for an upload. A
  fifth reader gets the next source's copy, or 404.
- **SpiNorFlash** reads the JEDEC ID and works out the size, using 4-byte
  addresses above 16 MB (MX25L256, W25Q256). It clears block protection
  at `begin()`, or for SST26 sends its global unlock. A chip whose status
  register is locked reports `WriteProtected`. Waits for the chip yield
  through `Config::yield`, with a timeout. It is blocking, not a state
  machine: a file system wants a read finished before it carries on.
  Run it on the web server's threads, not an ISR.
- **FatFs settings.** `FF_USE_LFN` 2 or 3 (long names; 1 shares one
  static buffer between threads). `FF_FS_REENTRANT` can stay 0 when only
  these classes use the card, since the iLock serialises them; set it to
  1 if other code uses FatFs too. `FF_FS_LOCK` can be 0, because the class
  tracks its own open files. Without long names only 8.3 names work, and the build
  warns. `FF_FS_TINY` 0: each open file holds its own 512-byte buffer.
- **Wiring.** The flash chip can share the W5500's SPI bus with its own
  CS. Hold `/WP` and `/HOLD` high (or use quad-capable parts in single-SPI
  mode). On an F207, an SD card works best on SDIO with CubeMX's
  `sd_diskio.c`. On an L432, use SPI with a `user_diskio.c`.
- **Size** (Cortex-M3, `-Os`). Flash: LittleFS 18.6 KB, FatFs 11 KB with
  long names, `SpiNorFlash` 1.1 KB, each file class 1.7 KB,
  `HttpFileAdmin` 1.9 KB plus its page 5.9 KB. RAM: `LittleFsNor` 0.8 KB,
  `HttpLittleFsFiles` 1.8 KB, `HttpFatFsFiles` 3.2 KB, plus FatFs's
  `FATFS` 0.6 KB.
- **Build.** `-DLITTLEFS_DIR=<littlefs v2.9>` builds `inet_littlefs`
  (LittleFS with `LFS_NO_MALLOC`). FatFs comes with the CubeMX project:
  add `storage/src/HttpFatFsFiles.cpp` to it. `-DFATFS_DIR` only builds
  the host test.
- **A browser opens more than one connection.** Chromium keeps two open
  to one server, so give `xHttpServer` `maxClients` of at least 2 (3 with
  anything else on it). Connections beyond that are refused while the
  others stay open.

## HTTPS and logins

```cpp
// Once, at start-up (an STM32F207 with its RNG enabled in CubeMX; build
// with MBEDTLS_DIR and INET_TLS_HARDWARE_RNG, and compile
// hw/stm32/src/TlsStm32Rng.cpp into the application).
tlsUseFreeRtosHeap();
static TlsFreeRtosLock tlsLock, authLock;

MbedTlsServer::Config t;
t.certPem = webCertPem;                 // from tools/make_web_cert.sh
t.keyPem = webKeyPem;
t.sessions = 2;
t.lock = &tlsLock;
static MbedTlsServer tls(t);
tls.begin();

static const WebUser users[] = {        // from tools/web_user.py
    {"ann", WebRole::Operator, 20000, {/* salt */}, {/* hash */}},
};
WebAuth::Config a;
a.users = users;  a.userCount = 1;
a.verify = WebPassword::verify;
a.random = MbedTlsServer::randomFn;  a.randomCtx = &tls;
a.now = [](void*) { return static_cast<uint32_t>(xTaskGetTickCount() * portTICK_PERIOD_MS); };
a.lock = &authLock;
static WebAuth auth(a);

xHttpServer::Config w;
w.port = 443;  w.maxClients = 2;  w.tls = &tls;
static xHttpServer web(eth, w);
auth.attach(web);                       // /api/login, /api/logout, /api/session
// ... routes; a handler that changes something asks
//     auth.require(req, res, WebRole::Operator, true)

static HttpsRedirect toHttps(443);      // and port 80 only sends browsers on
static xHttpServer plain(eth, plainConfig);
plain.get("/*", HttpsRedirect::handler, &toHttps);
```

**TLS** (`tls/MbedTlsServer`, mbedTLS 3.6 with `tls/config/inet_mbedtls_config.h`):
- TLS 1.2 only, with ECDHE-ECDSA and AES-128/256-GCM or
  ChaCha20-Poly1305: forward secrecy, nothing older. The key exchange
  uses X25519 or P-256, and the certificate is ECDSA P-256. curl,
  OpenSSL and Chromium all connect; TLS 1.0/1.1 and non-forward-secret
  suites are refused.
- Session tickets: a returning browser skips the expensive key exchange.
- Memory: a connection's buffers (24 KB at the peak, measured on a PC)
  come from the FreeRTOS heap while it is open, and all go back at close.
  The server keeps about 4 KB in between. `sessions` caps how many
  connections are open at once. A third browser waits, or is closed and
  retries.
- Flash: about 74 KB on a Cortex-M3 for TLS and PBKDF2 (mbedTLS's
  lookup tables in flash, not RAM).
- Speed: the F207 has no crypto accelerator, so each new connection's
  key exchange is estimated at about a second. That hasn't been measured
  on the chip.
- Randomness: `TlsStm32Rng.cpp` feeds mbedTLS from the MCU's true random
  number generator. On a PC, the operating system's.
- Certificates: `tools/make_web_cert.sh` makes an authority of your own,
  once, and a certificate per device. Install the authority's `ca.crt` on
  the operators' machines and browsers trust every device without a
  warning. Keep `ca.key` offline. The device key is compiled into the
  firmware: turn on the flash's read protection (RDP level 1) so it can't
  be read out over SWD.

**Logins** (`http/WebAuth`):
- Passwords are stored only as PBKDF2-HMAC-SHA256 hashes, salted
  (`tools/web_user.py`). The iteration count sets how slow each check is.
- A login returns a session cookie: 32 random bytes, `HttpOnly`,
  `SameSite=Strict`, `Secure`. It also returns a CSRF token, which every
  state-changing request must send back as `X-CSRF-Token`, and a foreign
  `Origin` is refused.
- Roles: viewer, operator, admin. `require()` answers 401 or 403 itself.
- Sessions end after 15 minutes idle or 8 hours in all (both
  configurable). The least recently used one makes way when all 8 slots
  are in use.
- Five wrong passwords lock that user out for a minute, doubling up to 15
  minutes. An unknown user name costs as much time as a wrong password,
  so names can't be guessed by timing.
- By default, logins are refused unless they come over HTTPS.

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
one that passes. It checks the INT line and the PHY, then brings up DHCP,
or the ESP module, or the ATWINC1500 (one per build: `-DBRINGUP_WINC=ON`
puts it on SPI1 in the W5500's place, and the log says why it stopped if
it does), looks up a name and sets the time. It logs each step
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
- `HttpClient_test`: response lexing whole, a byte at a time and in random
  pieces (Content-Length, chunked with extensions and trailers, to the
  close, 1xx, HEAD/204/304, and malformed responses), the response reader
  (buffer, truncation, streaming and stopping, headers, keep-alive rules),
  URLs, and the request head.
- `xHttpClient_test`: `xHttpClient` over the W5500 driver, against a
  simulated HTTP server at the far end of the chip's connections. It
  covers GET and POST by name and by address, chunked and to-close
  bodies, 100 Continue, HEAD, keep-alive, a 20 KB body truncated and
  streamed, stopping a stream, kept connections going stale (a GET sent
  again, a POST not), and every error: timeout, malformed, unknown host,
  https, bad URL and refused.
- `xHttp_test`: `xHttpServer` on real threads over the W5500 driver, with
  simulated browsers on the chip's far end. It covers a thread created per
  connection, pipelining, a form arriving slowly in pieces, a 14 KB
  streamed page, 413, three clients at once (slow handlers in parallel
  without holding up a fast one, nothing listening while all are busy),
  a closed connection freeing its slot, idle and request timeouts, a
  thread that can't be created (503), and `stop()` ending every thread.
- `HttpFiles_test`: files from memory and from a real folder through
  `FILE*`: index.html, types, gzip, a fallback, three sources, HEAD, a
  70 KB file, a file changed while serving, hidden files, and every way
  of trying to leave the folder.
- `HttpFileAdmin_test`: uploads in pieces of raw bytes, commit, list and
  delete through the real connection and `WebAuth`. It covers who may
  (no login, an operator, no CSRF token, a foreign Origin), wrong offsets
  and sizes, `maxFileBytes`, a full store, every kind of bad path, a file
  being read (503, or a wait with the file held), JSON escaping of
  names, the audit log, and the page.
- `SpiNorFlash_test` (with `-DLITTLEFS_DIR`): the driver against a
  simulated chip (`test/sim/SimSpiNor.h`): W25Q, MX25L256 with 4-byte
  addresses, SST26, protection, no chip, a stuck bus and a chip that
  stays busy. Then LittleFS on it: a 50 KB upload in pieces, listing,
  space, serving, a remount, a power cut at every write of a file
  update (the old or new file, never corrupt), readers with an uploader
  on several threads, and slow readers of a file replaced 60 times (each
  read one version, whole; every commit gets through).
- `HttpFatFs_test` (with `-DFATFS_DIR`): FatFs R0.15 with long names on
  a RAM disk. It covers uploads into new folders, commit, gzip serving,
  hidden files, listing and space, `kMaxOpen`, a card pulled out and put
  back, readers with an uploader on several threads, and a file replaced
  under slow readers.
- `StorageWeb_test` (with `-DMBEDTLS_DIR -DLITTLEFS_DIR`): the whole
  path over HTTPS with curl. The built-in page is served from an empty
  flash; an operator is refused; an admin uploads 20 KB in pieces and
  commits; a slow download during the next commit gets the old page
  whole; it is served and survives a remount; a delete brings the
  built-in page back. `StorageWeb_test --serve 120` stays up for
  `storage/test/files_ui_test.cjs`, which drives `/files.html` in
  headless Chromium (Playwright): login, uploads with odd names and
  folders, delete, a `..` refused, dark mode.
- `SocketNetDevice_test`: the stack on the operating system's sockets,
  over real TCP on the loopback, driven by curl. It covers pages from a
  folder, gzip, HEAD, POST, keep-alive, three slow requests in parallel
  with a fourth waiting in the backlog, `xHttpClient` to the same
  server, 1 MB echoed through `xClient`, refused and closed connections,
  and DNS and SNTP against the simulated servers on 127.0.0.3:53 and
  127.0.0.4:123 (skipped without root).
- `Lwip_test` (with `-DLWIP_DIR=<lwIP source>`): lwIP built for the PC, two
  interfaces on its loopback netif, everything through lwIP's own TCP and
  UDP. It covers `xHttpServer` to `xHttpClient` (an 84 KB chunked page,
  keep-alive, three slow requests at once), 256 KB echoed through
  `xClient`, DNS and SNTP, and the link going down and up.
- `WebAuth_test`: `HttpJson`, and logins as JSON and forms. Also the
  cookie's attributes, roles, CSRF tokens and Origin, idle and absolute
  expiry, logout, the lockout and its doubling, timing for unknown names,
  refusal over plain HTTP, eviction when full, and eight threads at once.
- `Tls_test` (with `-DMBEDTLS_DIR`): HTTPS over the host's sockets, checked
  with curl and `openssl s_client`. It covers PBKDF2 test vectors and
  `tools/web_user.py`'s output, certificate checking, TLS 1.2 with the
  intended suites and X25519, refusal of TLS 1.0/1.1, plain HTTP and
  non-forward-secret suites, ticket resumption, the port 80 redirect, a
  login and a protected write, 111 KB streamed, two connections at once,
  and the heap per connection.
- `Winc1500_test`: the ATWINC1500 driver against a simulated module
  (`test/sim/SimWinc1500.h`) that answers the SPI byte by byte as
  Microchip's driver expects, with its registers, boot handshake, HIF and
  firmware (access points, DHCP, sockets, DNS, the clock). It covers
  start-up with and without a reset pin, the CRC left off from before,
  0 to 5 idle bytes before each answer (and too many), no module, another
  chip, firmware too old or needing a newer driver, firmware that never
  starts, a refusing bus; WPA2 and open joins, a wrong passphrase, no such
  network, a bad passphrase refused before sending, switching networks,
  static addresses, DHCP renewal, RSSI, losing the network and rejoining by
  itself; TCP both ways (10 KB up, 20 KB down), a peer's close after its
  last bytes, refused and unanswered connects; a slow reader beside a fast
  one; listening, a second listener on the same port, a peer with nobody
  listening, the listener's linger, a third port refused; DNS (including
  a replaced lookup and an address), the time before and after the
  module's SNTP sync; failed transfers and corrupted commands mid-transfer;
  a module slow or unable to hand out buffers; a module reset behind the
  driver's back; and data arriving through IRQN.
- `WincRef_test` (with `-DWINC_REF_DIR`): Microchip's own driver against
  the same simulated module (see the driver's section).
- `xNet_test`: the real FreeRTOS-layer sources on real threads, against a
  FreeRTOS simulation (`test/stub`), with both drivers. It covers blocking
  reads and their timeouts, 20 KB each way, a close waking a sleeping
  reader, connect refusal and timeout, the socket limit, listen/accept, the
  INT pin, DHCP, and `xWifi` joining and moving data through the ESP driver,
  and through the ATWINC1500 driver (IRQN relayed as the interrupt, DNS,
  the module's time, 5 KB up and 8 KB down, a peer's close).
  It also covers `resolve()` from two threads at once and after a timeout,
  the time set by itself and by `syncTime()`, and DHCP's NTP server.
- `spi_block_transport_test` (in itransport): the two SPI phases under one
  chip-select, chained from the interrupt, with one wake-up per transfer.
