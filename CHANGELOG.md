# Changelog

All notable changes to this project are recorded here. The format is
based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
There are no tagged releases yet, so changes are grouped by date and
pull request, newest first.

## Unreleased

### 2026-10-08

#### Added
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
