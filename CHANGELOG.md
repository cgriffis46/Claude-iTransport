# Changelog

All notable changes to this project are recorded here. The format is
based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
There are no tagged releases yet, so changes are grouped by date and
pull request, newest first.

## Unreleased

### 2026-10-10

#### Added
- A LoRaWAN 1.0.4 Class A end device in `iRadio/lorawan` (US915, The
  Things Network) ([#26](https://github.com/cgriffis46/Claude-iTransport/pull/26)):
  - `lorawan::Mac`: OTAA join (DevNonce saved before each request,
    JoinNonce checked, RP002 join backoff), unconfirmed and confirmed
    uplinks, downlinks in RX1 and RX2 timed from the TxDone interrupt,
    32-bit frame counters, the MAC commands (LinkADR, DutyCycle,
    RXParamSetup, DevStatus, RXTimingSetup, LinkCheck, DeviceTime) and
    ADR with its backoff. The session is kept in an `iSessionStore` the
    application provides.
  - `LoRaWanFrame` (frames, keys, MIC, encryption), `Region` and
    `RegionUS915` (sub-band 2 for TTN), `xLoRaWanMac` (the CMSIS-RTOS2
    loop), and `lora::iLoRaRadio`, which `rfm95` now implements.
  - Tests `lorawan_frame_test` (against the lora-packet library and
    LoRaMac-node's formulas) and `lorawan_mac_test` (a simulated TTN
    gateway and network server).
- An RFM95W / SX1276 LoRa radio driver and the AES used by LoRaWAN, in
  `iRadio` ([#25](https://github.com/cgriffis46/Claude-iTransport/pull/25)):
  - `rfm95/`: `rfm95<TTransport>` (non-blocking, header only) and
    `xrfm95` (CMSIS-RTOS2): transmit, RX single (a symbol timeout) and
    RX continuous, each request with its own frequency, spreading
    factor, bandwidth, coding rate, I/Q inversion and power; events
    stamped by the DIO interrupts in `iClock` ticks; faults and restart
    on a stuck TX, RX or bus. `SX1276Regs.h` and `LoRaPhy.h` (FRF,
    time on air, RSSI, SNR).
  - `lorawan/`: `Aes128` and `AesCmac` (RFC 4493), for the LoRaWAN
    Class A MAC that comes next.
  - Tests `rfm95_test` (simulated SX1276s sharing an air) and
    `aes_cmac_test` (FIPS-197 and RFC 4493 vectors).
- A debug log and debug pins for testing on hardware with a logic
  analyser ([#24](https://github.com/cgriffis46/Claude-iTransport/pull/24)):
  - `iTransport/itransport/inc/DebugLog.h`: text lines with a sequence
    number and the time on any `iTransport` UART, non-blocking (a ring
    filled under a short lock, so interrupts may log, and sent by
    `dbg::poll()`), dropped lines counted; debug pins through the port.
    `ITRANSPORT_DEBUG` (CMake) chooses 0 off, 1 faults, 2 events, 3
    trace; at 0 the macros are empty.
  - Hooks in `SensorStateMachine` (state changes and failures of every
    driver, `setDebugTag()`), `BusTransport` (transfers, failures in the
    interrupt, the transfer and interrupt pins), `ublox_gps` and
    `mtk3339` (configuration and module events) and `DualChannelLink`
    (faults and the partner's state).
  - `hw/stm32/inc/Stm32DebugPort.h`: HAL_GetTick, a PRIMASK lock and
    BSRR pins.
  - `tools/saleae/saleae_log.py`: turns Saleae Async Serial and digital
    CSV exports into one timeline, with dropped lines and faults marked.
  - Tests `debug_log_test` and `tools/saleae/test_saleae_log.py`.
- `isensor/mtk3339`: a MediaTek MT3339 GNSS module driver (Adafruit
  Ultimate GPS, GlobalTop PA6H/PA1616S, CDTop), on a UART. ([#23](https://github.com/cgriffis46/Claude-iTransport/pull/23))
  - `Pmtk.h`: PMTK314 (sentences), PMTK220 (output interval), PMTK300
    (fix interval) and GlobalTop's `$PGCMD,33` antenna report, built
    without printf.
  - `mtk3339<TTransport>` and `xmtk3339` (CMSIS-RTOS2): waits for each
    `$PMTK001` and reports a refusal or no answer while carrying on,
    configures again when the module announces a restart
    (`$PMTK010,001`), reads the antenna status (`$PGTOP` or `$PCD`), and
    invalidates the data when the module falls silent.
  - Host test `mtk3339_test` (a simulated module).
- `isensor/ublox_gps`: an NMEA 0183 parser and a u-blox GNSS receiver
  driver on a UART ([#22](https://github.com/cgriffis46/Claude-iTransport/pull/22)).
  - `NmeaParser`: byte at a time, checksum required, GGA, RMC, GLL, VTG,
    GSA, GSV and ZDA from any talker, NMEA 4.10 fields, position in
    1e-7 degrees with integer arithmetic, a sentence taken whole or
    not at all, no heap and no `strtod`.
  - `UbxProtocol.h`: UBX frames and checksum, ACK/NAK, and the
    configuration messages: UBX-CFG-MSG and UBX-CFG-RATE for u-blox 6
    to 8, UBX-CFG-VALSET for u-blox 9 and 10.
  - `ublox_gps<TTransport>` and `xublox_gps` (CMSIS-RTOS2): the
    interrupt fills a ring, `main()` parses; it chooses the sentences
    and rate, waits for each ACK, reports a NAK or no answer and carries
    on with the receiver's defaults, and invalidates the data and starts
    again when the receiver falls silent.
  - Host tests `nmea_parser_test` and `ublox_gps_test` (a simulated
    receiver).

#### Changed
- `NmeaParser` moved from `isensor/ublox_gps` to `isensor/nmea`, shared
  by both GNSS drivers, with `ByteRing` (the interrupt-to-thread ring,
  taken out of `ublox_gps`) and its own test. It now gives a
  proprietary sentence's address and fields to the driver. ([#23](https://github.com/cgriffis46/Claude-iTransport/pull/23))
  `ublox_gps_test` took over the UBX framing tests.
- `CLAUDE.md`: `ublox_gps` is now in STM32_Static_Lib_Src's sync script
  and its iSensor projects' include paths, so that open item is gone. ([#23](https://github.com/cgriffis46/Claude-iTransport/pull/23))

#### Fixed
- `rfm95`: an RX single gave up as "stuck" on a packet that began inside
  its window but lasted more than 100 ms past it (a join accept at SF12
  is 412 ms); the deadline now allows for a 255 byte packet ([#26](https://github.com/cgriffis46/Claude-iTransport/pull/26)).

### 2026-10-09

#### Added
- `safeTransport/DualChannelLink`: the heartbeat link between the two
  MCUs of a safety relay, each MCU one channel (one loopback UART, one
  UART to its partner). It is a `SafeInput` for the partner's channel,
  used with `SafeDevice` next to the MCU's own loopback. Sent on a timer
  from boot whatever the partner does, so the two can't deadlock at
  startup waiting on each other. Sequence counter, sender id and CRC-S3
  catch silence, a stuck or replaying line, lost or corrupted frames, a
  partner reset and a link wired to itself; a one-way break trips both
  sides. The lower id is primary, for reporting only. ([#20](https://github.com/cgriffis46/Claude-iTransport/pull/20))
- `safeTransport`'s first host test, `dual_channel_link_test` (two
  simulated MCUs), built with `-DSENSOR_FW_BUILD_TESTS=ON`. ([#20](https://github.com/cgriffis46/Claude-iTransport/pull/20))
- Raspberry Pi Pico and Pico 2 transports in `iTransport/itransport/hw/rp2040/`,
  on the Pico SDK, non-blocking and with no RTOS needed. They run on the
  RP2040 and on the RP2350 (Arm or RISC-V cores), sizing their tables
  from the SDK's per-chip counts (16 DMA channels and 4 DMA interrupt
  lines on the RP2350):
  - `PicoI2CTransport`: an interrupt-driven state machine on the I2C
    controller's FIFOs, for register reads and writes (repeated start)
    and command-style chips, up to the full 33 bytes. A NACK fails the
    transfer; `checkDevice()` probes with a one byte read.
  - `PicoSPITransport`: DMA for both directions, finished by the DMA
    interrupt (any of the chip's DMA lines), with a GPIO chip-select.
  - `PicoUartTransport`: the `iTransport` byte stream, received from
    the UART interrupt into the sink and sent by DMA.
  - `PicoSyncTransport<TBus>`: an optional Pico `mutex_t` per bus for
    sharing it between the two cores, tried without waiting.
  - `SENSOR_FW_HARDWARE=RP2040` builds them as `PicoTransportLibrary`
    from a Pico SDK project.
  - `pico_transport_test` and `pico_transport_test_rp2350`: the
    transports, unchanged, over a simulation of the chip's I2C, SPI,
    UART and DMA behind stand-in Pico SDK headers, once as an RP2040
    and once as an RP2350. ([#19](https://github.com/cgriffis46/Claude-iTransport/pull/19))

#### Changed
- `README.md` rewritten: what iTransport is and why it helps (one driver
  across buses and boards, non-blocking transfers, shared-bus
  arbitration, small porting surface, failure handling, host testing),
  the interfaces and platforms, the modules built on it, how a driver is
  written, how to build and test, and that nothing has run on hardware. ([#21](https://github.com/cgriffis46/Claude-iTransport/pull/21))
- `CLAUDE.md` now carries the original sensor_fw design record: the
  working principles, the safeTransport and PLCTransport designs, the
  F207 safety relay's hardware and watchdog decisions, the CIP facts
  checked against the ODVA specs, and the open CIP Safety questions.
  It also notes that `CipSafeRelayUartLoopback.cpp` and itransport's
  `Stm32UartItCallbacks.cpp` both define the HAL UART callbacks. ([#20](https://github.com/cgriffis46/Claude-iTransport/pull/20))
- `CLAUDE.md`: the F207 is now planned as the central zone controller or
  PLC, with the safety relay possibly on a smaller STM32; the deleted
  `STM32F207ZG_SafeRelay` project was a blank template, to be recreated in
  its own repository. ([#20](https://github.com/cgriffis46/Claude-iTransport/pull/20))

#### Fixed
- `isensor` configures for microcontroller targets again: `pbmp280_driver`,
  the Linux BMP280 variant, is only built where POSIX threads exist,
  instead of failing the whole configure on toolchains without them. ([#19](https://github.com/cgriffis46/Claude-iTransport/pull/19))

### 2026-10-08

#### Added
- The web UI on SPI flash or an SD card, uploaded from a browser, so it
  can change without new firmware. ([#18](https://github.com/cgriffis46/Claude-iTransport/pull/18))
  - `iNetTransport/storage/`: `SpiNorFlash`, a SPI NOR flash driver over
    `iBlockTransport` (JEDEC ID and size, 4-byte addresses above 16 MB,
    block protection cleared, SST26 unlock, pages, 4 KB erase).
    `LittleFsNor` runs LittleFS on it with static buffers.
    `HttpLittleFsFiles` and `HttpFatFsFiles` (FatFs, for an SD card) serve
    the files and take uploads. `LITTLEFS_DIR` builds LittleFS, and
    `FATFS_DIR` builds the FatFs host test.
  - `http/HttpFileStore`: uploads arrive in pieces into a hidden
    `.name.part`, and the commit puts the file in place (atomically on
    LittleFS). A file being read is never replaced or removed under its
    reader. The store holds it closed to new readers until those reading
    it finish.
  - `http/HttpFileAdmin`: `/api/files` to list, upload (`PUT ?offset=`),
    commit (`POST ?size=`) and delete. It needs an admin session and the
    CSRF token, checks paths, enforces a size limit, waits for readers
    (or answers 503 with Retry-After), and audits every change.
    `httpFileAdminPage` (`/files.html`) is its page in the firmware.
  - `inc/iLock.h`: one mutex interface for the file systems, WebAuth and
    the TLS server.
  - Tests: `SpiNorFlash_test` (the driver against `test/sim/SimSpiNor.h`;
    LittleFS with power cuts at every write), `HttpFatFs_test` (FatFs on a
    RAM disk), `HttpFileAdmin_test`, and `StorageWeb_test` (uploads over
    HTTPS with curl, and `--serve` for `storage/test/files_ui_test.cjs` in
    Chromium). ([#18](https://github.com/cgriffis46/Claude-iTransport/pull/18))
- HTTPS and logins for the web server, for the STM32F207 on its own
  Ethernet.
  - `iNetTransport/tls/`: `MbedTlsServer`, a TLS 1.2 server on mbedTLS 3.6
    behind a new seam (`inc/iTls.h`), so `xHttpServer` gets HTTPS from
    `Config::tls` without seeing mbedTLS. It offers only ECDHE-ECDSA with
    AES-GCM or ChaCha20-Poly1305, uses X25519 or P-256 for the key
    exchange, and supports session tickets. The random generator and
    ticket keys are shared under a lock; the record buffers are taken
    from the heap per connection.
  - Also in `tls/`: `config/inet_mbedtls_config.h` (sized for the F207),
    `mbedtls.cmake` (mbedTLS built from `MBEDTLS_DIR`), `WebPassword`
    (PBKDF2-HMAC-SHA256), `TlsFreeRtos.h` (locks, and mbedTLS's heap on
    FreeRTOS), and `hw/stm32/src/TlsStm32Rng.cpp` (entropy from the MCU's
    RNG).
  - `http/WebAuth`: logins and sessions. Passwords are checked by hash; the
    session cookie is HttpOnly, SameSite=Strict and Secure; roles are
    viewer, operator and admin. Writes need a CSRF token and an
    acceptable Origin. Sessions expire when idle and after a maximum age.
    Repeated failures lock the user out, doubling each time. Timing
    doesn't reveal which user names exist, and logins are HTTPS-only by
    default.
  - `http/HttpJson` (top-level members of a small JSON body) and
    `http/HttpRedirect` (`HttpsRedirect`: port 80 sends browsers on).
  - `PlcTagWebApi` writes: `POST /api/tags/<name>`, for the configured
    role with the CSRF token, to allow-listed tags within their limits,
    each one audited. Values are checked against the tag's type, with
    64-bit integers exact. `readRole` can require a login to read. The
    built-in page gained a login and Set controls.
  - Tools: `tools/make_web_cert.sh` (your own certificate authority and
    device certificates, ECDSA P-256) and `tools/web_user.py` (WebUser
    entries).
  - Tests: `WebAuth_test` (54 checks), `Tls_test` (31: HTTPS over real
    sockets with curl and openssl s_client), the write checks in
    `PlcTagWebApi_test` (46 in all), and `PlcWebSecure_test` (17: end to
    end over HTTPS). The built-in page was driven in headless Chromium
    (`web/test/ui_test.cjs`, 17 checks; not in ctest). `Tls_test` and
    `WebAuth_test` pass under TSan and ASan/UBSan (mbedTLS built with
    them too), and `Tls_test` passed 16 runs four at a time.
  - Compiled for Cortex-M3 against the F2 HAL, FreeRTOS and lwIP headers:
    HTTPS with PBKDF2 is about 74 KB of flash. A connection took 24 KB of
    heap at its peak on a PC. Not run on an F207.
  ([#17](https://github.com/cgriffis46/Claude-iTransport/pull/17))
- The web server and clients on lwIP: `iNetTransport/sockets/`.
  - `SocketNetDevice`, an `iEthernetDevice` on a BSD socket API: lwIP's
    (`INET_SOCKETS_LWIP`; an STM32F207 with its own MAC, or an ESP32), or
    the operating system's. `xEthernet`, `xClient`, `xHttpServer`,
    `xHttpClient` and `xMqttClient` run on it unchanged. Sockets that
    listen on one port share one listening socket, so connections wait in
    its backlog. DNS and SNTP use `DnsClient` and `SntpClient` on a UDP
    socket of its own. It polls with one zero-timeout `select()`.
  - `hw/lwip/LwipNetif`: the link and address of an lwIP netif, DHCP
    included.
  - `NetSockets.h` undoes lwIP's `LWIP_COMPAT_SOCKETS` macros, which would
    otherwise rewrite `connect()` and `poll()` (found compiling with
    CubeMX-like options).
  - Host tests: `SocketNetDevice_test` (24 checks, real sockets, curl,
    DNS/SNTP simulated on loopback) and `Lwip_test` (18 checks, lwIP 2.2.1
    built for the PC, with `-DLWIP_DIR`). Both pass under ASan/UBSan, and
    `SocketNetDevice_test` also under TSan. Compiled for Cortex-M3 with
    lwIP's FreeRTOS port. Not run on an F207 or an ESP32.
  ([#16](https://github.com/cgriffis46/Claude-iTransport/pull/16))
- `http/HttpFiles`: the web UI from files instead of the firmware.
  `HttpStaticFiles` (index.html, types by extension, `.gz` when the
  browser takes gzip, `no-cache`, a fallback source, `..` refused) over
  `HttpFileSource`: `HttpStdioFiles` (`FILE*`) and `HttpMemoryFiles`.
  Host test `HttpFiles_test` (17 checks).
  ([#16](https://github.com/cgriffis46/Claude-iTransport/pull/16))
- `PLCTransport/web/`: the tag database as read-only JSON (`PlcTagWebApi`:
  `GET /api/tags`, `?names=`, `/api/tags/<name>`; every CIP elementary
  type, STRUCT as hex) and a built-in page that polls it
  (`PlcWebDefaultPage`). Values are copied under the registry's lock a few
  tags at a time and written out after it's released. Host tests
  `PlcTagWebApi_test` (18 checks) and `PlcWebServer_test` (8 checks, end
  to end over sockets, with curl and Python's JSON parser). PLCTransport gained
  `SENSOR_FW_BUILD_TESTS` and a README with the web security plan.
  ([#16](https://github.com/cgriffis46/Claude-iTransport/pull/16))
- HTTP/1.1 client in `iNetTransport/`, for a node sending its data out.
  - `HttpLexer` has a Response mode. It handles status lines, chunked
    bodies (decoded; extensions and trailers skipped), bodies that run to
    the close (`endOfInput()`), 1xx responses, and no body after HEAD
    (`expectNoBody()`) or for 204 and 304. New token types: `Status` and
    `Reason`.
  - `http/HttpClientProtocol`: `HttpUrl` (http and https URLs, ports,
    queries; fragments and user info refused), `httpWriteRequestHead()`,
    and `HttpResponseReader`. The reader is the lexer's sink. It reads the
    status, passes headers to a callback, and puts the body into a buffer
    (truncated, NUL-terminated if room) or streams it to a callback that
    can stop it. It also decides whether the connection can be kept.
  - `hw/freertos/xHttpClient`: `get()`, `post()` and `request()` on the
    caller's thread, with a timeout. It looks up names and keeps the
    connection alive. A GET on a kept connection that went stale is sent
    again, but a POST is not. Errors are reported by kind. It is 788
    bytes on a Cortex-M4 and allocates nothing. No TLS.
  - Host tests: `HttpClient_test` (45 checks) and `xHttpClient_test` (39
    checks, over the W5500 driver against a simulated HTTP server). Both
    pass under ASan/UBSan, and `xHttpClient_test` also under TSan; it
    passed 20 runs under load. Compiled for Cortex-M4 as C++14. Not run on
    hardware or against a real server.
  ([#16](https://github.com/cgriffis46/Claude-iTransport/pull/16))
- HTTP/1.1 web server in `iNetTransport/`, for GET, POST and the other
  methods, on either interface.
  - `http/HttpLexer`: cuts the request stream into fixed-size tokens
    (method, target, version, header name and value, body, end), a byte at
    a time, so a request can arrive in any pieces. It checks the syntax
    and frames bodies by Content-Length. When the token queue is full it
    stops and resumes.
  - `http/HttpConnection`: a state machine that builds each request from
    the tokens into one buffer per client. It routes the request
    (`HttpRoutes`: exact paths and prefixes, HEAD to GET, 404 and 405 with
    Allow) and makes sure it is answered. It also answers malformed,
    oversized and unsupported requests itself (400, 408, 413, 414, 417,
    431, 501, 505). It handles keep-alive, pipelining, HTTP/1.0 and
    `Expect: 100-continue`.
  - `http/HttpRequest` (decoded path, query, headers, body, `param()` for
    queries and urlencoded forms) and `http/HttpResponse` (`send()`, or a
    streamed `begin()`/`write()`/`printf()`, sent chunked to HTTP/1.1
    clients).
  - `hw/freertos/xHttpServer`: a daemon thread that accepts and creates a
    thread for each client. That thread lexes into a FreeRTOS queue,
    parses and runs the handlers. It handles idle and request timeouts,
    up to `maxClients` at once, 503 when no thread can be created, and a
    `stop()` that ends every thread.
  - Host tests: `Http_test` (52 checks) and `xHttp_test` (43 checks, real
    threads over the W5500 driver, simulated browsers on the chip's far
    end). `xHttp_test` passes under ASan/UBSan and TSan and passed 20
    runs under load. Compiled for Cortex-M4 as C++14. Not run on
    hardware or against a real browser.
  - The FreeRTOS host stand-in (`test/stub`) gained `xTaskCreate` (a
    std::thread), `vTaskDelete`, counting semaphores and `xQueueReset`.
  ([#16](https://github.com/cgriffis46/Claude-iTransport/pull/16))
- MQTT 3.1.1 client in `iNetTransport/`, QoS 0 and 1, no TLS.
  - `mqtt/MqttClient`: the protocol as pure logic, allocating nothing.
    It covers CONNECT (credentials, will, keepalive, clean session),
    publish, subscribe and unsubscribe, keepalive with PINGREQ, and
    timeouts for CONNACK and PINGRESP. Unacknowledged QoS 1 messages are
    kept and resent with DUP after a reconnect, including ones published
    while disconnected. Subscriptions are sent again after a reconnect.
    The decoder streams input and skips packets too big for its buffer.
  - `hw/freertos/xMqttClient`: the client on any `xNetInterface`
    (`xEthernet`, `xWifi`). Its own thread looks the broker up, connects
    and reconnects with backoff. Publishing from any thread goes straight
    out under the session's mutex, and a QoS 1 publish can wait for its
    ack. Received messages go to a FreeRTOS message buffer for a blocking
    `receive()`, or to a callback.
  - Host tests: `MqttClient_test` (45 checks) against
    `test/sim/SimMqttBroker.h`, and `xMqtt_test` (80 checks) on real
    threads over the W5500 driver, with `SimW5500` handing its TCP
    connections to the broker. Both pass under ASan/UBSan and TSan, and
    `xMqtt_test` passed 20 runs under load. Compiled for Cortex-M4 as
    C++14. Not run on hardware or against a real broker.
  - The FreeRTOS host simulation (`test/stub`) gained recursive mutexes,
    binary semaphores, message buffers, `pvPortMalloc` and
    `xTaskGetCurrentTaskHandle`. `SimW5500` gained `onTcpConnect` and
    `onTcpSend` hooks.
  ([#15](https://github.com/cgriffis46/Claude-iTransport/pull/15))
- `iRadio/`: a new module for radios, starting with a receiver for the
  Davis Vantage Pro2 / Vue ISS on an RFM69 (SX1231).
  - `davis_protocol`: the US, AU, EU and NZ hop tables, exact transmit
    intervals, bit reversal, the Davis CRC (direct and repeated),
    `decode()` for every known message type, `DavisSchedule` (which
    channel to listen on and until when, for up to eight stations, with
    discovery, missed-packet tracking and loss after 50 misses) and
    `DavisWeather` (a station's latest readings and rain total).
  - `davis_rfm69<TTransport>`: the receiver as a non-blocking state
    machine. Checks the chip version, configures it for Davis, reads the
    sync word back, retunes per the schedule, and reads PayloadReady from
    DIO0 or by polling.
  - `xdavis_rfm69`: the receiver in its own FreeRTOS task. Packets go out
    through a queue, text lines through a stream buffer, and commands come
    in through a queue. DIO0 wakes the task through a task notification.
  - Host tests: `davis_test`, `davis_rfm69_test` (against a simulated
    RFM69 and ISS stations with real airtime) and `xdavis_rfm69_test`
    (over a single threaded FreeRTOS stand-in). Compiled for Cortex-M4F
    and Cortex-M0+ against the FreeRTOS V11.1.0 headers. Not run on
    hardware.
  ([#14](https://github.com/cgriffis46/Claude-iTransport/pull/14))
- `iClock` (itransport): a free running counter to time things by, readable
  from an interrupt. `Stm32RtcClock`: the STM32 RTC as one, from its
  calendar and subsecond counter on the LSE crystal (`PREDIV_S + 1` ticks
  a second), and `timestamp()` for the RTC's timestamp unit. Host test
  `stm32_rtc_clock_test` against a simulated RTC; compiled against ST's
  HAL headers for the F407, L432 and L476
  ([#14](https://github.com/cgriffis46/Claude-iTransport/pull/14)).
- The Davis receiver timed by a clock: `davis_rfm69::setClock()` (the
  schedule then counts in the clock's ticks), `onDio0At()` /
  `onDio0FromISRAt()` for a time latched in hardware (DIO0 on RTC_TS),
  `DavisPacket::rxTicks`, `DavisSchedule::shift()` and `expired()`. A clock
  set while running is seen against the RTOS tick and the schedule moved
  with it; an unexplained jump makes it start over. In the host test, with
  the CPU clock 1 % fast, 1 % of packets are received on the RTOS tick and
  all of them on the RTC
  ([#14](https://github.com/cgriffis46/Claude-iTransport/pull/14)).
- `SPITransport::setAddressBit()`: `AddressBit::WriteHigh` for chips that
  set bit 7 of the address to write (Semtech SX1231 / RFM69, SX127x).
  `ReadHigh`, the old behaviour, stays the default. Five new checks in
  `itransport_test`
  ([#14](https://github.com/cgriffis46/Claude-iTransport/pull/14)).
- `iDisplay/hd44780`: a non-blocking HD44780 character LCD driver (16x1
  up to 40x2 and 20x4) behind an I2C port expander, the PCF8574 board or
  Adafruit's MCP23008 backpack, with `xhd44780` for CMSIS-RTOS2. It is an
  `iTextSurface` and an `iDisplayDevice`, so the GUI runs on it
  unchanged. It sends only the characters that changed, keeps the
  datasheet's power-up, reset and clear waits, and recovers by itself,
  custom characters included. Backlight, display on/off and eight custom
  characters. Host test with 39 checks against a simulated LCD that
  flags timing and E-edge setup violations
  ([#13](https://github.com/cgriffis46/Claude-iTransport/pull/13)).
- `iTextSurface::showEditCursor()`: `xTextField` marks the cell it is
  editing, which a character LCD shows with its blinking cursor, as it
  cannot draw inverse
  ([#13](https://github.com/cgriffis46/Claude-iTransport/pull/13)).
- `iDisplay`: long press and auto-repeat buttons. `xButtonConfig` makes a
  button plain, long press (a short press is `Pressed` on release, a long
  one is `Held`, never both) or auto-repeat (`Pressed`, then `Repeat`
  while held). `xGuiButtonGroup` runs the periodic CMSIS-RTOS2 timer that
  posts `Held` and `Repeat` to the GUI's queue. Menus and fields step on
  `Repeat`, and holding Enter is back in a menu and cancel in a field.
  `gui_test` now has 128 checks
  ([#13](https://github.com/cgriffis46/Claude-iTransport/pull/13)).
- `iDisplay/`: a new module for displays and a GUI to run on them.
  - `display_core`: `iTextSurface` (a grid of character cells, so the
    same menus run on a graphic or character display), `MonoCanvas` (a
    1 bit frame buffer in the SSD1306 page layout with pixels, lines,
    rectangles and text), a 5x7 font with a degree sign, and the
    `iDisplayDevice` interface.
  - `ssd1306/`: a non-blocking SSD1306 128x64 / 128x32 driver on
    `SensorStateMachine`, over I2C (`ssd1306`) or 4-wire SPI with a D/C
    pin (`ssd1306_spi`), and `xssd1306` for CMSIS-RTOS2. It sends only the
    pages that changed, and powers up again and repaints by itself
    after a failure. Host test with 47 checks.
  - `gui/`: `xScreen`, `xGuiCore` (the screen stack), `xMenu` /
    `xMenuScreen`, `xYesNoField`, `xChoiceField`, `xTextField` and
    `xButton` (debounce, safe in an interrupt), ported from the
    `xDisplay` classes in FeatherM0_Davis_ISS_Ethernet. Fixed on the
    way: menu scrolling, submenus, text field bounds, and the release
    check of two of the buttons.
  - `hw/freertos/`: `xGui`, the GUI task, and `xGuiButton`
    (CMSIS-RTOS2). Buttons only post to a one-event queue, and the task
    only acts on events from it. Host test with 86 checks over a
    simulated queue.
  - Compiled for Cortex-M4 and Cortex-M0+ with `arm-none-eabi-g++ 13.3`
    (`-std=gnu++14 -fno-exceptions -fno-rtti`). Not run on hardware.
  ([#13](https://github.com/cgriffis46/Claude-iTransport/pull/13))
- `iNetTransport/dns/DnsClient`: a DNS client written as pure logic, for
  one A-record lookup at a time. It checks the reply's ID and question,
  follows CNAMEs and retries three times over 10 s. Host test with 27
  checks
  ([#12](https://github.com/cgriffis46/Claude-iTransport/pull/12)).
- `iNetTransport/sntp/SntpClient`: an SNTP client written as pure logic.
  It checks a random nonce, rejects kiss-o'-death and unsynchronised
  servers, halves the round trip and handles the 2036 rollover. Host test
  with 22 checks
  ([#12](https://github.com/cgriffis46/Claude-iTransport/pull/12)).
- `xNetInterface::resolve()` (DNS, sleeping for the answer; `"a.b.c.d"`
  answered at once), `syncTime()`, `timeValid()` and `unixTimeMs()`. The
  time is set as soon as there is an address and kept by re-syncing every
  `Config::ntpIntervalMs` (an hour by default). It uses the NTP server
  DHCP names, else `Config::ntpServer` (`pool.ntp.org`)
  ([#12](https://github.com/cgriffis46/Claude-iTransport/pull/12)).
- `iNetDevice::resolve()` / `requestTime()`, and
  `iNetDeviceHost::resolved()` / `timeReceived()`. The W5500 driver runs
  DnsClient and SntpClient on its UDP service socket. The ESP-AT driver
  uses the module's `AT+CIPDOMAIN` and SNTP (`AT+CIPSNTPCFG`,
  `AT+CIPSNTPTIME?`)
  ([#12](https://github.com/cgriffis46/Claude-iTransport/pull/12)).
- DHCP asks for and keeps the network's NTP server (option 42), in the
  new `NetConfig::ntp`. `IpAddress::parse()` and `format()`
  ([#12](https://github.com/cgriffis46/Claude-iTransport/pull/12)).
- Bring-up firmware: a DNS lookup and the time after the address, UTC on
  the stats line, and an RFC 868 time service on TCP 37. `net_bringup.py`
  compares the board's clock with the PC's
  ([#12](https://github.com/cgriffis46/Claude-iTransport/pull/12)).

#### Changed
- `HttpStaticFiles` takes up to three sources, tried in order, so storage
  can come before the built-in pages. It refuses hidden paths (any part
  starting with `.`), where uploads in progress are kept, so `/..a` is
  now refused too. ([#18](https://github.com/cgriffis46/Claude-iTransport/pull/18))
- `WebAuth::Lock` and `MbedTlsServer::Lock` are now `iLock`, and
  `TlsFreeRtosLock` is an `iLock`. Code that derives from either still
  builds. `inet_http` puts `iNetTransport/inc` on the include path for
  it. ([#18](https://github.com/cgriffis46/Claude-iTransport/pull/18))
- `xHttpServer::Config::tls`: HTTPS. With it, the client thread does the
  handshake first. `Stats::tlsFailures` counts failed handshakes, and
  `HttpRequest::secure()` says a request came over TLS
  (`HttpConnection::setSecure()`).
  ([#17](https://github.com/cgriffis46/Claude-iTransport/pull/17))
- `PlcTagRegistry::snapshot()`: copies tags out under the lock (by page or
  by name, without making a `std::string`), for code that mustn't hold the
  lock while it works.
  ([#16](https://github.com/cgriffis46/Claude-iTransport/pull/16))
- `HttpRoutes::on()`, the same as `add()`, so route-adding code takes an
  `xHttpServer` or an `HttpRoutes`.
  ([#16](https://github.com/cgriffis46/Claude-iTransport/pull/16))
- The STM32L432KC bring-up firmware builds one network interface, the
  W5500 or the ESP module: `BRINGUP_ETH` and `BRINGUP_WIFI` both on (or
  both off) now stops the build. An L432 board has one interface. One
  leaves about 15 KB of RAM free beyond the 34 KB heap; both left 10 KB.
  ([#16](https://github.com/cgriffis46/Claude-iTransport/pull/16))
- `xNetInterface::toTicks()` is public, for classes built on an interface
  such as `xMqttClient` ([#15](https://github.com/cgriffis46/Claude-iTransport/pull/15)).
- `iDisplay`: `kDegreeChar` moved from `Font5x7.h` to `iTextSurface.h`,
  as every surface uses it
  ([#13](https://github.com/cgriffis46/Claude-iTransport/pull/13)).
- W5500: socket 7 is now a UDP service socket for DHCP, DNS and SNTP,
  open whenever it is kept back, with a static address too.
  `w5500_param_t::dhcp` is now `serviceSocket`, `dhcpSeed` is now `seed`,
  and `w5500_dhcp_socket` is now `w5500_service_socket`
  ([#12](https://github.com/cgriffis46/Claude-iTransport/pull/12)).
- `iNetDevice` has two more pure virtual functions, so a chip driver
  written outside this repository needs `resolve()` and `requestTime()`.
  They may simply return false
  ([#12](https://github.com/cgriffis46/Claude-iTransport/pull/12)).

### 2026-10-07

#### Added
- `CHANGELOG.md`, and a rule in `CLAUDE.md` to keep it up to date with
  every change ([#11](https://github.com/cgriffis46/Claude-iTransport/pull/11)).
- `CLAUDE.md`: the layout, how drivers are written and tested, build
  commands, the related STM32_Static_Lib_Src libraries, history and
  open items ([#10](https://github.com/cgriffis46/Claude-iTransport/pull/10)).
- `isensor/mmc56x3/`: non-blocking driver for the MEMSIC MMC5603 /
  MMC5633 magnetometer, with one-shot and continuous modes, set/reset
  coil pulses, the bandwidth, a temperature reading, and an
  `xmmc56x3` FreeRTOS variant. Host test with 40 checks
  ([#9](https://github.com/cgriffis46/Claude-iTransport/pull/9)).
- `isensor/lsm303dlhc/`: non-blocking drivers for the ST LSM303DLHC.
  `lsm303dlhc_accel` runs 12 bit with block data update; `lsm303dlhc_mag`
  checks the "H43" ID and reports an overflowed axis as NAN. The two
  halves are separate I2C devices and can share a bus. `x*` FreeRTOS
  variants. Host test with 47 checks
  ([#9](https://github.com/cgriffis46/Claude-iTransport/pull/9)).
- `iNetTransport/` (from the inet work,
  [#7](https://github.com/cgriffis46/Claude-iTransport/pull/7)):
  - `iNetDevice`: the seam between a network chip driver and the
    interface on it.
  - `xNetInterface`, `xEthernet`, `xWifi`, `xClient` for FreeRTOS: a
    driver thread, a queue inbox, per-socket stream buffers, and event
    groups to sleep on with timeouts.
  - `w5500<TTransport>`: non-blocking WIZnet W5500 driver (SPI), with
    TCP client and server, the INT pin or polling, and recovery from
    bus failures.
  - `espat<TTransport>`: ESP32/ESP8266 ESP-AT Wi-Fi driver over a UART,
    with multiple links, passive receive and rejoin after a module
    reboot.
  - `dhcp/DhcpClient`: a DHCP client written as pure logic, run by the
    W5500 driver on socket 7.
  - An echo server example and bring-up firmware for the STM32L432KC,
    and host tests on simulated chips and a FreeRTOS simulation.
- itransport: `iBlockTransport`, `SpiBlockTransport` and
  `Stm32HalSpiBlockTransport` for block transfers of any length under
  one chip select (DMA when available), and `HAL_SPI_RxCpltCallback` in
  `Stm32SpiItCallbacks.cpp` ([#7](https://github.com/cgriffis46/Claude-iTransport/pull/7)).

#### Changed
- The PLC tag database (`PlcTagRegistry`, `PlcTagDescriptor`,
  `PlcDataType`, `PlcMutex`), `CipFrame`, `CipTagMessageCodec` and
  `CipTagTcpServer` moved from `safeTransport/` into a new top-level
  `PLCTransport/`. It has its own CMake build, and
  `PLC_MUTEX_USE_CMSIS_RTOS2` is now an option
  ([#8](https://github.com/cgriffis46/Claude-iTransport/pull/8)).
- Folders renamed without their date suffixes: `iTransport_20261006`
  is now `iTransport`, and `safeTransport_20261004` is now
  `safeTransport` ([#6](https://github.com/cgriffis46/Claude-iTransport/pull/6)).
- `isensor/` and all the sensor driver folders moved to the top of the
  repository. `isensor` builds on its own and adds itransport from
  `ITRANSPORT_DIR`
  ([#4](https://github.com/cgriffis46/Claude-iTransport/pull/4)).
- Each sensor driver's member definitions moved from its header into
  `src/<Name>.tpp`, which the header includes at its end. The code is
  unchanged ([#3](https://github.com/cgriffis46/Claude-iTransport/pull/3)).
- Every folder has its own `CMakeLists.txt`.
  `SENSOR_FW_HARDWARE=HOST`, `ITRANSPORT_BUILD_WIRINGPI` and
  `SENSOR_FW_BUILD_TESTS` were added, so the host tests run under
  `ctest`. `STM32_PROJECT_INCLUDE_DIR` was added for the CubeMX `main.h`
  that the STM32 transports include
  ([#2](https://github.com/cgriffis46/Claude-iTransport/pull/2)).
- `safeTransport` now builds against itransport instead of its own
  copies of the transports, with a new `CMakeLists.txt` for the safety
  code ([#2](https://github.com/cgriffis46/Claude-iTransport/pull/2)).

#### Removed
- `archive/` and the `STM32F207ZG_SafeRelay` STM32CubeIDE project. Both
  are still in git history
  ([#5](https://github.com/cgriffis46/Claude-iTransport/pull/5)).
- Duplicate files in `safeTransport`: the 38 that matched the archive
  exactly, and the older `iTransport.h`, `Stm32HalUartTransport` and
  `Stm32UartItCallbacks.cpp`
  ([#2](https://github.com/cgriffis46/Claude-iTransport/pull/2)).

#### Fixed
- 16 files that the CMake build expects but the 2026-10-06 import left
  out were restored from the archive: `SensorBase`, `BMP280Sensor`,
  `Barometer`, `Altimeter`, `Thermometer`, `xBMP280`, `pBMP280`,
  `IFileTransport` and `WiringPiTransport.h`
  ([#2](https://github.com/cgriffis46/Claude-iTransport/pull/2)).

### 2026-10-06

#### Added
- The reorganised itransport, imported from the iTransport project
  chat ([#1](https://github.com/cgriffis46/Claude-iTransport/pull/1)):
  - `BusTransport`, with `I2CTransport` and `SPITransport` built on it.
  - `FreeRtosTransport<TBus>`, which replaces `FreeRtosI2CTransport`.
  - `OneWireUartTransport`, and STM32 HAL transports for UART and
    1-Wire.
- `SensorStateMachine` and 11 non-blocking sensor drivers as class
  templates over their transport: AHT20, BME280, BMP280, DS18B20,
  HMC6352, HTU21DF, LPS35HW, MPL3115A2, PM25, SHT31 and Si7021. Each has
  an `x*` FreeRTOS variant and a host test
  ([#1](https://github.com/cgriffis46/Claude-iTransport/pull/1)).

### 2026-10-04

#### Added
- `safeTransport_20261004`: the Safe interface, safety devices and
  zones, CIP Safety placeholders, the PLC tag registry and CIP tag
  server, CAN and UART safety transports, and the
  `STM32F207ZG_SafeRelay` CubeIDE project.
- `README.md`.

#### Changed
- The first iTransport code moved into `archive/iTransport_20261004`.

### 2026-08-18

#### Added
- First iTransport code: `ISensorTransport`, `I2CTransport`,
  `SPITransport`, STM32/Arduino/WiringPi/KR260 transports, `SensorBase`,
  and the BMP280 driver with its FreeRTOS (`xBMP280`) and POSIX
  (`pBMP280`) variants. The MIT `LICENSE` was added on 2026-08-21.
