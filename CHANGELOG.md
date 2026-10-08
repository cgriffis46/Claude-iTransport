# Changelog

All notable changes to this project are recorded here. The format is
based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
There are no tagged releases yet, so changes are grouped by date and
pull request, newest first.

## Unreleased

### 2026-10-08

#### Added
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
- `xNetInterface::toTicks()` is public, for classes built on an interface
  such as `xMqttClient`.
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
