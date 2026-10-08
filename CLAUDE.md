# CLAUDE.md

Non-blocking, asynchronous embedded drivers for STM32 (and Arduino /
Linux), written in C++ and tested on a PC. Owner: cgriffis46.

## Layout

```
iTransport/itransport/   transports: the seam between "how we talk to a chip"
                         and "what the chip means"
isensor/                 sensor base classes and one folder per sensor driver
iNetTransport/           network interfaces (W5500 Ethernet, ESP-AT Wi-Fi,
                         DHCP, DNS, SNTP)
PLCTransport/            PLC tag database, CIP tag codec, CIP tag TCP server
iDisplay/                displays (SSD1306) and a GUI: screens, menus,
                         fields, buttons, a CMSIS-RTOS2 GUI task
iRadio/                  radios: the Davis ISS receiver on an RFM69, with
                         a FreeRTOS task (queues, stream buffer)
safeTransport/           safety: Safe interface, devices, zones, relays, CAN,
                         CIP Safety placeholders, events
```

Each module has the same shape: `inc/`, `src/`, `lib/` (build output,
git-ignored), and `hw/<platform>/{inc,src}` for stm32, freertos,
arduino and linux. Sources use flat `#include "Foo.h"`; each folder's
`CMakeLists.txt` puts the right `inc/` folders on the include path.

### itransport
- `ISensorTransport` (register and command access: `writeReg`,
  `writeRegs`, `readRegs`, `writeBytes`, `readBytes`, `isBusy`,
  `lastOpFailed`, `checkDevice`). Every call only *starts* a transfer.
- `iTransport` (byte streams, e.g. UART), `iTransportOneWire`,
  `iBlockTransport` (SPI block/DMA, used by the W5500).
- Bus classes: `BusTransport`, `I2CTransport`, `SPITransport`,
  `SpiBlockTransport`, `OneWireUartTransport`.
- `iClock` (`inc/iClock.h`): a free running counter (`ticksPerSecond()`,
  `now()`, ISR-safe). `hw/stm32/Stm32RtcClock`: the RTC (LSE) as one,
  `PREDIV_S + 1` ticks a second, from SSR/TR/DR read with interrupts
  masked; `timestamp()` converts the RTC_TS timestamp unit's latch. Host
  test against `test/stub_hal_rtc/`.
- `hw/freertos/FreeRtosTransport<TBus>`: CMSIS-RTOS2 mutex and thread
  flags, shared by I2C/SPI/1-Wire.
- `hw/stm32/`: `Stm32HalI2CTransport`, `Stm32HalSPITransport`,
  `Stm32HalSpiBlockTransport`, `Stm32HalUartTransport`,
  `Stm32HalOneWireTransport`, and the HAL interrupt callbacks
  `Stm32{I2C,Spi,Uart}ItCallbacks.cpp`. These callbacks override the
  HAL's weak ones, so they must be compiled into the application, never
  pulled from a static library. The STM32 headers include the
  application's CubeMX `main.h`.

### isensor drivers
`aht20 bme280 bmp280 DS18B20 HMC6352 htu21df lps35hw lsm303dlhc mmc56x3
mpl3115a2 PM25 sht31 si7021`, plus `SensorBase`/`BMP280Sensor` (the
older style) and `SensorStateMachine`.

### iDisplay
- `display_core` (`inc/`, `src/`): `iTextSurface` (character cells and
  a cursor), `MonoCanvas` (1 bit frame buffer in SSD1306 page layout,
  also an `iTextSurface` of 6x8 cells), `Font5x7`, and
  `iDisplayDevice` (`text()`, `requestFlush()`, `main()`, `idle()`,
  `failed()`), which every display driver provides.
- `ssd1306/`: `ssd1306<TTransport, H>` (I2C; the control byte is the
  "register" of `writeRegs`), `ssd1306_spi` (D/C pin callback,
  `writeBytes`), `xssd1306*` (osDelay sleeps). Written like a sensor
  driver on `SensorStateMachine`. It sends only pages whose hash changed.
- `hd44780/`: `hd44780<TTransport, COLS, ROWS>` behind a PCF8574 (`writeBytes`) or
  MCP23008 (IOCON.SEQOP, then `writeRegs(GPIO, ...)`) backpack, 4-bit
  mode, three expander bytes per nibble. Diffs a shadow character
  buffer. Shows the text field's cell with the blinking cursor
  (`iTextSurface::showEditCursor`). Waits are rounded up a tick
  (1 ms ticks can be short by up to 1 ms). `xhd44780` sleeps with osDelay.
- `gui/` (no RTOS): `xScreen`/`xNavigator`, `xGuiCore` (screen stack,
  home at the bottom), `xMenu`/`xMenuScreen`, `xYesNoField`,
  `xChoiceField`, `xTextField`, `xButton` (ISR-safe debounce; plain,
  long press or auto-repeat via `xButtonConfig`).
- `hw/freertos/`: `xGui` (the GUI task), `xGuiButton`, and
  `xGuiButtonGroup` (periodic osTimer that posts `Held`/`Repeat`; a pin
  interrupt cannot start a CMSIS-RTOS2 timer), CMSIS-RTOS2 only. The owner's design rule: buttons only post to the GUI's event
  queue, which holds one event (that is the flow control: presses
  arriving while it is full are dropped), and the GUI task only acts on
  events from that queue (no timer, no polling; data threads post
  `Refresh`). Screens specific to an application stay in the
  application (e.g. FeatherM0_Davis_ISS_Ethernet's weather screens).

### iRadio
- `davis/` pure logic (`davis_protocol` library): `DavisProtocol` (hop
  tables, `intervalSixteenths()`, `reverseBits()`, `crc16()`,
  `checkCrc()`, `decode()`), `DavisSchedule` (per-station sync, misses,
  discovery; times in 1/16 ms, signed differences), `DavisWeather`.
- `davis_rfm69<TTransport>` (header only, `davis/src/davis_rfm69.tpp`):
  a `SensorStateMachine`; register writes are queued as ops and run one
  transfer each. It sets `SPITransport::AddressBit::WriteHigh` on an
  SPITransport (the RFM69 sets bit 7 to write) and reads the sync word
  back after configuring. Good packets go to the virtual `deliver()`.
  `setClock(iClock*)` times packets by that clock (Stm32RtcClock) instead
  of nowMs; `onDio0At(ticks)` takes a hardware-latched time. Clock steps
  (the RTC set) are found by comparing the clock with nowMs and
  `DavisSchedule::shift()`ed. The schedule counts in the clock's ticks
  (`begin(..., ticksPerSecond)`); callers re-plan when `expired()`.
- `hw/freertos/xdavis_rfm69.h` uses native FreeRTOS (queue, stream
  buffer, task notifications), like iNetTransport's hw/freertos;
  CMSIS-RTOS2 has no stream buffer. `sleep()` is `ulTaskNotifyTake()`,
  woken by `onDio0FromISR()` and by commands.
- `test/sim/SimDavis.h`: RFM69 at register level plus ISS stations with
  6.7 ms airtime and 1 ms RX settling; `test/stub/` is a single threaded
  FreeRTOS in which time passes only inside `ulTaskNotifyTake()`.

## How a sensor driver is written

Follow an existing driver (`lps35hw` for registers, `HMC6352` for
command-style chips, `mmc56x3`/`lsm303dlhc` for the newest):

- `template <typename TTransport> class foo : public SensorStateMachine<TTransport, foo_state_t>`.
  The driver *inherits* its transport; everything after `param` in the
  constructor goes to the transport's constructor:
  `xfoo<Stm32HalI2CTransport> s(param, &hi2c1, FOO_ADDR, i2c1Mutex);`
- `static_assert` that `TTransport` derives from `ISensorTransport`
  (or whichever interface it needs).
- `main(nowMs)` is a `switch` over states. Every bus access is two
  states: one that issues it (`issued(this->readRegs(...), wait_state, nowMs)`)
  and one that waits (`if (landed(nowMs)) {...}`). Never block or spin.
- Buffers the transport fills must be members, not locals.
- Write-only chip registers: build the value each time; never read
  back and modify.
- Timeouts: `issued`/`landed` carry the bus timeout; long polls use a
  `phase_start` timeout and `fail()`. The error state waits for
  `errorCleared(nowMs, backoff)`, then restarts from init.
- `onFail()` sets readings to NAN. Readings are NAN until the first
  one. `newData()` and getters clear the flag.
- The done state sleeps at most `request_poll_ms` (100 ms) at a time so
  `startMeasurement()` is seen promptly; `sleep()` is a no-op stub,
  overridden by the `x*` FreeRTOS variant with `osDelay()`.
- Files: `inc/foo.h` (class), `src/foo.tpp` (member definitions,
  included at the end of the header by relative path `../src/foo.tpp`,
  and including `../inc/foo.h`), `inc/xfoo.h` (RTOS variant),
  `test/foo_test.cpp` + `test/stub/cmsis_os2.h`, `CMakeLists.txt`
  defining `foo_sensor` (INTERFACE) and the test. Add the folder to the
  `foreach` list in `isensor/CMakeLists.txt`.
- Tests simulate the chip (`MockChip`) behind a bare `ISensorTransport`
  (`MockBus`) and behind the real `I2CTransport` with the HAL calls
  faked (`LoopI2C`); cover absent/wrong chips, stuck and refused
  transfers, stalled measurements, tick rollover, and the `x*` sleep
  times.
- Code must build as C++14 (`-std=gnu++14 -fno-exceptions -fno-rtti`),
  which is what STM32CubeIDE uses, even though CMake asks for C++17.

## Building and testing

Every module builds on its own on a PC. Use these flags for host tests:

```
cmake -S <isensor|iTransport|iNetTransport> -B build \
      -DSENSOR_FW_HARDWARE=HOST -DITRANSPORT_BUILD_WIRINGPI=OFF -DSENSOR_FW_BUILD_TESTS=ON
cmake --build build && ctest --test-dir build
cmake -S safeTransport -B build -DSENSOR_FW_HARDWARE=HOST      # builds, no tests
cmake -S PLCTransport -B build                                  # plc_tags
cmake -S iDisplay -B build -DSENSOR_FW_HARDWARE=HOST \
      -DITRANSPORT_BUILD_WIRINGPI=OFF -DSENSOR_FW_BUILD_TESTS=ON   # ssd1306_test, gui_test
cmake -S iRadio -B build -DSENSOR_FW_HARDWARE=HOST \
      -DITRANSPORT_BUILD_WIRINGPI=OFF -DSENSOR_FW_BUILD_TESTS=ON   # davis tests
```

Last known results: isensor 19 tests (its 13 drivers plus itransport's
tests), iTransport 6, iNetTransport 12, iDisplay 9 (ssd1306_test,
hd44780_test, gui_test and itransport's 6), iRadio 9 (davis_test,
davis_rfm69_test, xdavis_rfm69_test and itransport's 6), all passing. Each test file also
has a one-line `g++` build command in its header.

`SENSOR_FW_HARDWARE` is `STM32` (default; needs `CMSIS_RTOS_INCLUDE_DIR`,
`STM32_HAL_INCLUDE_DIR`, `STM32_PROJECT_INCLUDE_DIR`), `ARDUINO`
(`ARDUINO_CORE_INCLUDE_DIR`) or `HOST`. `isensor`, `iNetTransport` and
`safeTransport` find itransport through `ITRANSPORT_DIR` (default
`../iTransport/itransport`). PLCTransport options:
`PLC_MUTEX_USE_CMSIS_RTOS2` (no `std::mutex` on the STM32 toolchain),
`FREERTOS_INCLUDE_DIRS`, `LWIP_INCLUDE_DIRS` for `cip_tag_server`.

For ARM checks, an xPack `arm-none-eabi-gcc` can be downloaded from
GitHub releases; compile with the CubeIDE flags above plus
`-mcpu=cortex-m4 -mfpu=fpv4-sp-d16 -mfloat-abi=hard -mthumb`.

## Related repository: cgriffis46/STM32_Static_Lib_Src

STM32CubeIDE static library projects for the STM32F407VG (F4) and
STM32L432KC (L4).

- `STM32{F4,L4}xx_iTransport_Static_Lib` builds itransport and the STM32
  HAL transports. It carries the HAL and `cmsis_os2.h` headers only, no
  HAL sources. `Callbacks/` holds the interrupt callbacks for the
  application to compile itself. `LibConfig/main.h` stands in for the
  application's `main.h` and must not go on an application's include
  path.
- `STM32{F4,L4}xx_iSensor_Static_Lib` carries isensor and all the
  drivers as headers. The `.a` only holds `SensorBase`, `BMP280Sensor`
  and `xBMP280`.
- `STM32L432KC_MMC56x3_Static_Lib` and `STM32L4xx_lsm303dlhc_static_lib`
  compile the new drivers for `Stm32HalI2CTransport`, bare metal and
  `x*` (`mmc56x3_stm32.h`, `lsm303dlhc_stm32.h`, which declare them
  `extern template`). Their workspace project names are
  `stm32L4xx_MMC56x3_static_lib` and `stm32l4xx_lsm303dlhc_static_lib`.
  Their old blocking classes were removed.
- The older PM25, SHT31, HTU21DF, xPM25, RTC and SCD30 libraries are
  untouched.
- **Updating:** this repo is the source of truth. Run
  `sh sync_from_claude_itransport.sh /path/to/Claude-iTransport` from
  that repo's root (update its `SENSORS` list when adding a driver);
  `SOURCE.txt` records the commit.
- **HAL rule:** `USE_HAL_{I2C,SPI,UART}_REGISTER_CALLBACKS` must be 0 in
  both the libraries and the application. The callbacks need the weak
  HAL ones, and the setting changes the HAL handle layout. The old
  xPM25 library needs UART = 1, so it can't share an application with
  these.
- Link order in an application: MMC56x3/LSM303DLHC library, then
  iSensor, then iTransport. The repo's README.md has the full setup.

## Working with the owner

- Develop on a `claude/...` branch, commit, push, then ask before
  opening a pull request. When asked, open it against `main` and merge
  it with a regular merge commit (not squash).
- Another Claude session sometimes merges into `main` at the same time
  (e.g. iNetTransport). Fetch `main` before starting work and before
  merging, and re-run the tests if it moved.
- Commit messages explain what changed and how it was verified. Never
  put a model name in commits, PRs or code.
- Every change that adds, changes, removes or fixes something gets an
  entry in `CHANGELOG.md`, in the same commit. Put it under
  `## Unreleased`, under today's date heading (add one if needed, newest
  first), and in the right group: Added, Changed, Removed or Fixed.
  Name what changed and link the pull request once it exists.
- Be explicit about what was not verified (hardware, STM32CubeIDE).

## History (October 2026)

1. Imported the work from the iTransport project chat (`itransport_update.zip`):
   the reorganised itransport, `SensorStateMachine`, and 11 header-only
   template sensor drivers with host tests.
2. Gave each folder its own `CMakeLists.txt`, added `HOST` builds and
   ctest, and restored the isensor/linux files the zip omitted.
3. Moved driver member definitions into `src/*.tpp`.
4. Moved the sensor folders into `isensor/` and made it a top-level
   module.
5. Removed `archive/` and the `STM32F207ZG_SafeRelay` CubeIDE project
   (both are still in git history: the old main.cpp, main_linux.cpp,
   conversation_transcript.pdf, and the F207 HAL/FreeRTOS/lwIP headers
   used for STM32 compile checks).
6. Dropped the date suffixes: `iTransport_20261006` became `iTransport`
   and `safeTransport_20261004` became `safeTransport`.
7. Moved the PLC tag database and CIP tag server out of safeTransport
   into `PLCTransport/`.
8. Another session added `iNetTransport/` (W5500, ESP-AT, DHCP,
   `SpiBlockTransport`).
9. Built the STM32_Static_Lib_Src iTransport/iSensor libraries.
10. Wrote non-blocking MMC56x3 and LSM303DLHC drivers and moved those
    two libraries onto them.
11. Added DNS and SNTP to iNetTransport (`DnsClient`, `SntpClient`,
    `xNetInterface::resolve()`/`unixTimeMs()`), on the W5500's service
    socket and through the ESP-AT module's own commands.

12. Added `iDisplay/`: the SSD1306 driver and the GUI classes, ported
    from FeatherM0_Davis_ISS_Ethernet's `.ino` (`xDisplay` became
    `xScreen`) onto CMSIS-RTOS2 for the eventual STM32 port.
13. Long press and auto-repeat buttons (`Held`, `Repeat`) through
    `xGuiButtonGroup`'s timer.
14. HD44780 character LCD driver (PCF8574 and MCP23008 backpacks).
15. Added `iRadio/`: the Davis ISS receiver on an RFM69, written new
    (the sketch's DavisRFM69 is CC-BY-SA), and
    `SPITransport::setAddressBit()` for Semtech radios.
16. `iClock` and `Stm32RtcClock`; the Davis receiver timed by the STM32
    RTC, with DIO0 latched by the RTC timestamp unit.

## Open items

- None of the drivers or libraries has run on hardware. The MMC56x3,
  LSM303DLHC and HMC6352 sequences come from datasheets only.
- The STM32CubeIDE projects were checked with arm-none-eabi-gcc using
  their `.cproject` settings, but have not been opened in CubeIDE.
- `safeTransport`: `CipSafetyCodec.h` and `CipSafetyBaseFormatCrc.h`
  are deliberate placeholders (encode/decode return false until
  written against ODVA's CIP Safety spec). `Stm32HalCanTransport` needs
  a CubeMX project with CAN enabled, and `SafeZoneJsonPersistence`
  needs nlohmann/json.
- `iDisplay`: the HD44780 driver covers the I2C backpacks only (not
  direct GPIO, not the 74HC595/SPI side). No inactivity timeout back to home (post `Home` from an
  application timer). FeatherM0_Davis_ISS_Ethernet's screens have not
  been moved onto it.
- `iRadio`: not run against an RFM69 or an ISS. `Stm32RtcClock` not run
  on a chip (compiled against the F407/L432/L476 HAL headers only). Repeater packets are
  delivered on request but never used for timing; no transmit.
- `iTransport/itransport/REMOVED.txt` is left over from the zip import;
  the files it names are already gone.
- `safeTransport/sensor_fw.zip` and its `*.html` files are old reference
  material.
