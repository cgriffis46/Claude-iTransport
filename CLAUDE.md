# CLAUDE.md

Non-blocking, asynchronous embedded drivers for STM32 (and Arduino /
Linux), written in C++ and tested on a PC. Owner: cgriffis46.
Targets: STM32 with FreeRTOS first (the F207 as the central zone
controller or PLC: tag server and web UI; the safety relay possibly on a
smaller STM32; the L432KC as a sensor/transmitter node), Pico; later a
KR260 as a zone controller (CIP and OPC-UA) and an RPi5 with ROS2.

## Working principles

Carried over from the original sensor_fw design sessions; keep applying them.

1. Verify against the real thing (datasheets, ODVA specs, the real
   vendor headers) and say plainly what is not verified. Anything taken
   from general knowledge is labelled so in a comment. Stub headers hid
   two real bugs once (a missing `lwip/tcpip.h`, no `std::mutex` on the
   ARM toolchain), so STM32 checks compile against the real headers.
2. Abstract interface, platform-specific implementation (`iTransport`,
   `Safe`, `SafeInput`, `SafeOutput`, `EventQueue`).
3. Inject, don't own: composing classes hold references or pointers to
   objects owned elsewhere, with the lifetime documented.
4. Never optimistically safe: an empty zone is unsafe, disagreeing
   channels are unsafe, boot is latched unsafe. Safety decisions go
   through `SafeInterlock`, never `GetSafe1State()`/`GetSafe2State()`.
5. Going unsafe is immediate and ungated; returning to safe needs an
   explicit action.
6. Explicit over convenient (`addMember()` needs a name and a code;
   `LightCurtain` needs two inputs).
7. Callbacks and output writes fire on real transitions, not every poll.
8. Processing is explicit and polled (`poll()`, `EvaluateSafe()`,
   `processEvents()` are called by the owner), not hidden async.
9. Off is safe (de-energize to trip).
10. Little-endian on the wire (CIP's order).
11. No dynamic allocation in embedded core classes where practical;
    exceptions are noted.
12. When a design fork is significant, describe it and ask before building.

## Layout

```
iTransport/itransport/   transports: the seam between "how we talk to a chip"
                         and "what the chip means"
isensor/                 sensor base classes and one folder per sensor driver
iNetTransport/           network interfaces (W5500 Ethernet, ESP-AT Wi-Fi,
                         lwIP or OS sockets, DHCP, DNS, SNTP), MQTT and HTTP
                         clients, a web server that serves files, and
                         storage/ for them (SPI flash + LittleFS, FatFs)
PLCTransport/            PLC tag database, CIP tag codec, CIP tag TCP server,
                         the tags as JSON for the web server (writes over
                         HTTPS with logins)
iDisplay/                displays (SSD1306) and a GUI: screens, menus,
                         fields, buttons, a CMSIS-RTOS2 GUI task
iRadio/                  radios: the Davis ISS receiver on an RFM69, with
                         a FreeRTOS task (queues, stream buffer); the
                         RFM95 (SX1276) LoRa radio; a LoRaWAN Class A MAC;
                         a MeshCore node
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
- `DebugLog.h` / `src/DebugLog.cpp`: the debug log and debug pins for
  testing on hardware (see "Debugging on hardware" below).
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
- `hw/rp2040/` (Raspberry Pi Pico and Pico 2: the RP2040, and the RP2350
  on its Arm or RISC-V cores; Pico SDK, no RTOS needed):
  `PicoI2CTransport` (an interrupt-driven state machine on the I2C
  controller's 16 entry FIFOs: register byte, repeated start, reads,
  NACK through TX_ABRT, finished on STOP_DET), `PicoSPITransport` (two
  DMA channels a transfer, finished by the DMA interrupt, GPIO
  chip-select), `PicoUartTransport` (the `iTransport` stream: RX from
  the UART interrupt, TX by DMA). Each installs its own shared
  interrupt handler, so there is no callbacks file. `PicoSyncTransport<TBus>`
  takes an optional `mutex_t*` for a bus used from both cores, tried
  but never waited on. Host test `test/rp2040/pico_transport_test.cpp`
  runs them over a simulation of the RP2040 peripherals behind
  stand-in Pico SDK headers (`test/rp2040/sim/`); built again with
  `SIM_RP2350` as `pico_transport_test_rp2350`. Size per-chip tables
  from the SDK's `NUM_DMA_CHANNELS`/`NUM_DMA_IRQS`/`NUM_I2CS`/`NUM_UARTS`
  (12/2 DMA on the RP2040, 16/4 on the RP2350), never a literal.

### isensor drivers
`aht20 bme280 bmp280 DS18B20 HMC6352 htu21df lps35hw lsm303dlhc mmc56x3
mpl3115a2 PM25 sht31 si7021 ublox_gps mtk3339`, plus `nmea` (what the
GNSS drivers share), `SensorBase`/`BMP280Sensor` (the older style) and
`SensorStateMachine`.

GNSS receivers on a UART (`iTransport`): `nmea/` holds what they share,
`ublox_gps/` and `mtk3339/` one driver each, built the same way.
- `nmea/NmeaParser` (pure logic, header-only like the drivers): one character
  at a time, checksum required, GGA RMC GLL VTG GSA GSV ZDA from any
  talker (GP GL GA GB/BD GQ GI GN), NMEA 4.10 fields (RMC nav status,
  GSA system ID, GSV signal ID). Position in 1e-7 degrees by integer
  arithmetic (no float loss, no double); empty fields NAN or invalid. A
  sentence is taken whole or not at all (fields before the satellites
  saved and put back; GSV checks first). Up to 96 characters: u-blox's
  high precision mode goes past NMEA's 82. Proprietary sentences
  (`$PMTK`, `$PGTOP`, `$PUBX`) come back as `Other`, read through
  `address()`/`field()` (valid until the next "$").
- `nmea/ByteRing<N>`: the interrupt -> thread ring both drivers use
  (atomic load/store only).
- `ublox_gps/UbxProtocol.h`: UBX frames, Fletcher checksum, ACK/NAK, `cfgMsg`,
  `cfgRate` (u-blox 6-8) and `ValSet` (CFG-VALSET, RAM layer, u-blox
  9-10). Numbers from Zephyr's and SparkFun's u-blox code, which agree;
  u-blox's PDFs could not be downloaded here.
- `ublox_gps<TTransport>`: the interrupt only fills a 512 byte SPSC
  ring; `main()` splits UBX from NMEA and parses. `ublox_gps_param_t`
  needs `ublox_config_t` (None, Legacy, ValSet): each CFG message waits
  for its ACK (3 tries, 500 ms); NAK or no answer sets `configStatus()`
  and it carries on with the receiver's defaults. Doesn't change the
  baud rate. Silence for `silenceMs` fails (data invalid), then it
  attaches and configures again. `xublox_gps` wakes on each line end
  and every 64 bytes (an ACK has no newline, so the wait state sleeps
  5 ms). About 1.2 KB of RAM an instance.
- Tests: `nmea_parser_test` (published GGA `*47`/RMC `*6A` and the
  quoted UBX "GLL off" `FB 11` / "5 Hz" `DE 6A` as outside checks) and
  `ublox_gps_test` (a simulated generation 8 or 10 receiver: ACK/NAK,
  rates applied, NMEA at the set rate and baud).
- `mtk3339<TTransport>` (MediaTek MT3339: Adafruit Ultimate GPS,
  GlobalTop PA6H/PA1616S, CDTop): `mtk3339_param_t(configure)`; sends
  PMTK314 (sentences; ZDA is field 17), PMTK220 (output, 100-10000 ms)
  and PMTK300 (fix, never under 200 ms: 5 Hz is the chip's limit), each
  waiting for `$PMTK001,cmd,3` (3 tries, 1 s); another flag sets
  `configStatus()` Rejected with `configFlag()`/`configCommand()`, and
  it carries on. `$PMTK010,001` (the module started: settings may be
  gone) makes it configure again. `antennaStatus` sends `$PGCMD,33,1`
  (no answer expected, own buffer) and reads `$PGTOP,11,x` (1 shorted,
  2 internal, 3 external) or CDTop's `$PCD,11,x` (1 internal, 2
  external, 3 shorted). No baud change (PMTK251): RMC+GGA at 5 Hz is
  about the limit at 9600. Commands built without printf (`pmtk::Body`).
  PMTK strings and checksums from Adafruit_GPS (the test checks them),
  PMTK314's field order from Adafruit's CircuitPython GPS docs, the
  PMTK001 flags from MediaTek's manual (3 = success seen in one other
  source only). `mtk3339_test`: a simulated module that ignores input
  while booting, announces itself, applies and ACKs, can answer late.

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
  tables, `intervalTicks()`, `reverseBits()`, `crc16()`, `checkCrc()`,
  `decode()`), `DavisSchedule` (per-station sync, misses, discovery;
  times in the clock's ticks, 1/16 ms by default, signed differences),
  `DavisWeather`.
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
- `rfm95/` (header only): `rfm95<TTransport>`, the RFM95W / SX1276 in
  LoRa mode, on `SensorStateMachine`. One request at a time, each with
  its own `lora::Config` (frequency, SF 7-12, bandwidth, CR, preamble,
  CRC, I/Q inversion, power), as LoRaWAN uses a radio. `transmit()`,
  `receive(cfg, symbols)` (RX single, 1-1023 symbols; 0 = RX
  continuous until `standby()` or the next request), `powerDown()`,
  `takeEvent()` (tx_done, rx_done with RSSI/SNR, rx_timeout, crc_error,
  fault; two 255 byte slots, overflow counted). Events are stamped in
  `onDio0/1()` (interrupt) or at the poll, in `iClock` ticks with
  `setClock()`. Register writes are queued ops (at most 26, measured;
  32 slots), whole values, never read-modify-write; the FIFO in 32 byte
  pieces. Startup checks RegVersion 0x12 and reads back the mode and
  sync word. Timeouts: TX at time on air + 200 ms, RX single at its
  window + 100 ms, then `fault` and a restart after 1 s. PA_BOOST only
  (2-17 dBm, 20 with PaDac 0x87, OCP 100/140 mA), capped by
  `maxPowerDbm`. 500 kHz errata (0x36/0x3A). LDRO when a symbol is over
  16 ms. Sync word 0x34 (LoRaWAN public, TTN). WriteHigh address bit on
  an SPITransport. `xrfm95`: CMSIS-RTOS2 thread flag 0x04000000. About
  1.5 KB of RAM. `SX1276Regs.h`, `LoRaPhy.h` (FRF = f*2^19/32 MHz,
  Semtech's time-on-air formula, RSSI -157 + r + r/16 (+ SNR if
  negative), SNR raw/4): values from Semtech's LoRaMac-node, checked
  against arduino-LoRa, not against the datasheet. DetectOptimize and
  DetectionThreshold are left at reset (their upper bits unverified).
  `rfm95` implements `lora::iLoRaRadio` (`rfm95/inc/iLoRaRadio.h`: the
  radio as the MAC sees it, `lora::Event`; `rfm95_event_t` and
  `rfm95_ev_*` are its names for them). An RX single's safety deadline
  allows for a 255 byte packet that began inside the window.
- `crypto/` (header only, `radio_crypto`): `Aes128` (both ways),
  `AesCmac` (RFC 4493), `Sha256` and `HmacSha256`; table based, not
  hardened against side channels. Checked against FIPS-197, RFC 4493,
  FIPS 180-4, RFC 4231 (and OpenSSL/hashlib on random inputs).
- `third_party/monocypher/`: Monocypher 4.0.2 (BSD-2 / CC0), vendored
  unchanged, `monocypher` (C99) library; only its Ed25519 is used.
- `lorawan/`: the `lorawan` library: `LoRaWanFrame`
  (1.0.x frames, keys, MIC, payload crypto; pure functions, checked
  against the lora-packet npm library), `Region` (what differs per
  region) and `RegionUS915` (subBand 2 = TTN; tables and LinkADRReq rules
  from LoRaMac-node v4.7.0), and `Mac`: LoRaWAN 1.0.4 Class A, OTAA,
  written here (not LMIC/LoRaMac-node). Injected: an `iLoRaRadio`, a
  `Region`, an `iSessionStore` (91 byte record with DevEUI and CRC).
  DevNonce saved before each join request (save fails: not sent);
  JoinNonce must increase; FCntUp saved ahead in `saveEvery` steps;
  FCntDown 32-bit and increasing. RX windows from the TxDone stamp with
  LoRaMac-node's `ComputeRxWindowParameters` (`Mac::rxWindow`); join
  backoff 1%/0.1%/0.01% (RP002); confirmed retries after RX2 + 1-3 s;
  ADR backoff (64/32, LoRaMacAdrCalcNext); sticky answers for
  RXParamSetup/RXTimingSetup; LinkADRReq blocks answered once each;
  NewChannel/DlChannel/TxParamSetup skipped without an answer.
  `main()` runs the radio too (one thread); `sleepHintMs()` and
  `xLoRaWanMac` (CMSIS-RTOS2, flag 0x08000000, `wakeFromIsr()` after the
  radio's `onDio0()`) sleep exactly as long as allowed. `Mac` 1.4 KB RAM.
- `meshcore/` (`meshcore` library): a MeshCore node (protocol v1, as in
  MeshCore firmware v1.12+, MIT, meshcore-dev/MeshCore). `MeshPacket`
  (header, transport codes, path with 1-3 byte hashes, payload <= 184,
  packet hash = SHA-256 of type + payload, first 8 bytes), `MeshCrypto`
  (AES-128-ECB with zero padding + 2 byte HMAC-SHA256 tag keyed with the
  32 byte padded secret; channel hash = SHA-256(key)[0]; the "Public"
  key), `MeshIdentity` (Ed25519 via Monocypher from a 32 byte seed;
  public keys starting 00/FF refused), `MeshMessages` (adverts: key,
  time, signature over key|time|app data, app data <= 32 bytes with
  flags/location/features/UTF-8 name; group text "sender: text" <= 160
  and group data type|len|data <= 165), and `Node` over any
  `iLoRaRadio`: RX continuous, duplicate table (64 hashes), adverts
  verified, group channels (4) opened, its own packets sent by flood
  through a queue of 3 with MeshCore's 50%/hour airtime budget and
  listen-before-talk (120-480 ms waits, forced after 4 s). Sync word
  0x12 is the radio's (rfm95_param_t::syncWord); US default 910.525 MHz
  SF7 62.5 kHz CR5, preamble 32 (16 above SF8), 17 dBm. Not a repeater;
  no direct messages, ACKs, paths or regions yet. A device runs either
  LoRaWAN or MeshCore. `xMeshNode` (CMSIS-RTOS2, flag 0x10000000).
  Node 2.7 KB RAM; ~23 KB flash with Ed25519 (M4, -O2); advert verify
  needs ~2.4 KB of stack (use a 4 KB task). Checked against MeshCore's
  own Packet.cpp, Utils.cpp and Identity.cpp built on the PC (900
  vectors; a sample in meshcore_packet_test).
- `iLoRaRadio` also has `channelBusy()` (a header heard in RX continuous,
  no RxDone yet; forgotten after a 255 byte packet's time) and `busy()`
  (a request queued or transfers under way: call main() again).
- `test/sim/SimSX1276.h`: SX1276s at register level sharing an `Air`
  (frequency, SF, BW, LDRO, sync word and I/Q must match; collisions;
  the LoRa bit only changes in sleep; RX single symbol timeouts; DIO0/1;
  faults: absent, stuck, refused, broken TX, corrupt CRC; a radio entering
  RX may still lock onto a preamble with 6 symbols left; ValidHeader
  raised after preamble + header; `inRange` for topologies), and a
  `sniff` hook for a gateway. `test/sim/SimLoRaWanServer.h`: a TTN gateway and
  network server for one device on sub-band 2, with its own US915 numbers
  and its own AES inverse cipher (join accepts are made by decrypting).
  `rfm95_test` (171 checks), `aes_cmac_test` (FIPS-197, RFC 4493),
  `lorawan_frame_test` (124), `lorawan_mac_test` (318),
  `meshcore_crypto_test` (41), `meshcore_packet_test` (72),
  `meshcore_node_test` (152: nodes, a test repeater, MeshCore-made
  packets, LBT, the budget, faults, `xMeshNode`).

### safeTransport (flat folder, builds on HOST; one test so far)
```
Safe (GetSafe1/2State, SetSafe1/2Callback)
 ├─ SafeInput  → TransportSafeInput, GpioSafeInput, UartLoopbackChannelSafeInput
 ├─ SafeOutput → TransportSafeOutput, GpioSafeOutput
 ├─ SafeDevice (2 SafeInputs + SafeOutput [+ optional reset SafeInput])
 │    └─ LightCurtain (same constructor, a named domain type)
 ├─ SafeZone   (aggregates Safe*; AND only)
 └─ UartLoopbackSafe (dual-channel loopback, polled with nowTicks)
SafeInterlock (static): evaluate() → Safe | Unsafe | Discrepancy; isFullySafe()
```
- `SafeDevice` starts latched unsafe. `reset()` clears the latch only if
  both raw inputs agree safe now. A `resetInput` false→true edge calls
  `reset()` (a button held at boot clears nothing). Output writes are
  change-gated, plus one forced write at construction.
- `SafeZone` holds `Safe*`, so zones nest and one signal (a light
  curtain) can be in many zones: a breach cascades to every robot zone
  while a robot's local fault stays in its own. `addMember(Safe&, name,
  safetyCode)`. AND only: configurable logic (OR etc., as in commercial
  zone controllers) is not built; the header marks where a `Logic` enum
  would go. `DiscoverSafeDevices()` is a no-op.
- `LightCurtain` keeps `SafeDevice`'s manual-reset latch (a bare
  `SafeInput` would recover by itself when the beam clears). A
  single-output curtain passes the same input twice, which is harmless
  but means `SafeInterlock` can never see a Discrepancy.
- `SafeZoneJsonPersistence` (Linux only, built when nlohmann/json is
  found): `{zone_name, devices:[{name, safety_code}]}`, written via temp
  file + rename. Reading gives a manifest only;
  `verifyMembersMatchManifest()` reports drift. Bad JSON or schema
  rejects the whole file. No schema version yet.
- Events are separate from Safe: Safe = must act now, boolean, latched;
  an Event is a graduated notice (e.g. nearing a no-fly zone). Don't
  stretch `Safe` into levels. `EventCode` is an open `uint16_t` (0–1
  reserved, ranges per domain such as 1000+ for drones); `Event` = code,
  timestamp, ≤8 bytes. `SimpleEventQueue<N>` is a ring buffer, not
  thread-safe, returns false when full. `SafeToEventBridge` pushes on
  transitions and sets no timestamps.
- Two-MCU relay: each MCU is one channel, with one UART as its own
  loopback (`UartLoopbackChannelSafeInput`) and one UART to the other
  MCU (TX to RX both ways) carrying a heartbeat. `DualChannelLink` is
  that link, and a `SafeInput` for the partner's channel:
  `SafeDevice(ownLoop, link, output, &resetButton)`. 11 byte frame
  (`A5 5A`, seq, 32-bit id, flags, CRC-S3), sent on a timer from boot
  whatever the partner does, saying only what this MCU measured, so
  the old startup deadlock (each waiting on the other's "safe") can't
  happen. The partner counts as safe while frames keep coming (under
  `timeoutTicks`), each seq is the last + 1 (a repeat, gap or jump back
  is a fault until `framesToTrust` in a row), the id isn't ours, and it
  reports its loopback good and that it hears us (so a one-way break
  trips both). Faults report unsafe at once, per frame. Lower id is
  `Primary`, for reporting only; safety stays symmetric. Interrupt to
  task through an SPSC ring (atomic load/store only, fine on an M0).
  For 4.17 ms at 1 ms ticks: poll and send every tick, timeout 3, link
  at ~1 Mbaud. Test: `test/dual_channel_link_test.cpp` (two simulated
  MCUs).
- CRCs: `CrcS3` (CRC-S3) and `CipSafetyBaseFormatCrc` (see CIP facts
  below); `CipSafetyCodec` is a placeholder.
- Also here: `Stm32L4SafetyRelay` (dual channel + IWDG), CAN
  (`Stm32HalCanTransport`, `Stm32CanSafetyBroadcaster`),
  `LinuxUdpTransport`, `xBNO085` (a scaffold on native FreeRTOS stream
  buffers; SHTP has no CRC check, reports not parsed).
- Design intent for drones and robots (not code): isolated safety zones
  per robot plus floor-wide shared zones; CAN safety relays inside each
  drone. LoRa is not a safety data path (duty cycle, airtime ≫ ms), fine
  for telemetry. ROS2's default middleware is DDS, whose Deadline and
  Liveliness QoS fit "silence = unsafe". Electrical target: 1/4 cycle at
  60 Hz ≈ 4.17 ms.

### Safety relay (first sketched on the Nucleo-F207ZG)
The `STM32F207ZG_SafeRelay` CubeIDE project was still a blank template;
it was deleted to tidy this repo and will be recreated in a separate
repository. The plan may change: the safety relay could go on a smaller
STM32, with the F207 as the central zone controller or PLC. The notes
below are from that template; `safeTransport/CipSafeRelay*.cpp` are the
relay code written for it, and carry over to whichever chip it lands on
(pins, UARTs and clock will change).
- Clock: 120 MHz HCLK, APB1 30 MHz, APB2 60 MHz. The PLL runs from HSI
  although the `.ioc` lists a 25 MHz HSE; switching to HSE is still to
  do. Ethernet is MII with a LAN8742 PHY, lwIP with `WITH_RTOS 1`.
- `CipSafeRelayUartLoopback.cpp` (written before the two-MCU design:
  two loopbacks on one chip): UART4/UART5 loopbacks at 115200 8N1
  need jumpers PA0↔PC11 and PC12↔PD2 (without them: unsafe forever).
  CubeMX generates the IRQ handlers; don't add them by hand. Objects are
  made with `new` in `InitUartSafetyLoopback()` from `USER CODE BEGIN 2`.
  Its `HAL_UART_*CpltCallback`s must be `extern "C"`. Placeholders: the
  pattern `{0xA5,0x5A,0x3C,0xC3}` (use the serial number) and
  `timeoutTicks=50` (untuned). `UartSafetyLoopbackIsSafe()` is not yet
  wired into a `SafeDevice`/`SafeZone`.
- IWDG ÷4, reload 4095 ≈ 512 ms at a nominal 32 kHz LSI (LSI can be off
  by 30–40%). `FeedIwdgIfSafe()` refreshes only while the loopbacks are
  safe; call it right after `PollUartSafetyLoopback()`. WWDG is off (its
  window was ~136 µs).
- The IWDG is the last resort for a hung MCU, not what meets the 1/4
  cycle response (that is `SafeOutput` following `SafeInterlock`). GPIOs
  float through reset, so the output stage must be off-is-safe in
  hardware (pull-down, energize to run). Under consideration: an
  external watchdog IC (TPS3813, MAX6369/6370 as starting points only)
  gating the output stage's power, kicked from a GPIO only while safe,
  ideally by a timer in toggle mode. A bootloader that sets a
  user-defined safe state is deferred; it's only needed if a site's safe
  state is "on".
- CubeMX workflow: the owner edits the `.ioc` and regenerates; our code
  stays in `USER CODE` sections.

### PLCTransport (CIP tag server)
1. `PlcTagRegistry` (+ `PlcDataType`, `PlcTagDescriptor`): register a
   variable or struct by name, type from `PlcTypeTraits<T>`
   (`registerStructTag()` for structs). Live pointers, one mutex.
   Rejects duplicate names, writes to read-only tags and wrong sizes.
2. `CipTagMessageCodec::handleRequest()`: a Message Router request in, a
   response out; Get/Set_Attribute_Single on one symbolic segment only.
   Always answers (with an error status if need be). Not a general router.
   Uses `std::vector`, so it needs a heap.
3. `hw/freertos/CipTagTcpServer` (lwIP raw API): callbacks run in
   `tcpip_thread` and must not block, so `tcp_recv` fills a per-slot
   accumulator, whole frames go to that slot's stream buffer and
   `{slot, generation}` to one shared queue; one worker task runs the
   codec; `tcpip_callback()` sends the reply from `tcpip_thread`. 8 fixed
   slots (extra connections `tcp_abort()`ed in `onAccept`). `tcp_arg` is
   the slot's own address, because `tcp_err_fn` gets no pcb. The
   generation counter stops a reply landing on a reused slot. One
   request in flight per connection (a pipelining client is aborted).
   Start it from `MX_LWIP_Init()`'s `USER CODE BEGIN 3`. Checked only
   with `-fsyntax-only` against the real F207 headers.
4. Not started: `PlcTagClient<TTransport>` limited to Ethernet/Wi-Fi
   transports (needs an `iTransportWifi` marker).
- `CipFrame` (2-byte LE length prefix) only delimits messages on TCP; it
  is not EtherNet/IP encapsulation (that needs ODVA Vol. 2).
- `PlcMutex` is an `osMutex` with `PLC_MUTEX_USE_CMSIS_RTOS2`, else
  `std::mutex` (the ARM toolchain is "Thread model: single").
- OPC-UA is deferred: open62541 with a thin adapter over the registry,
  "None" security first, on the KR260. STM32s speak CIP only.

### CIP facts (checked in ODVA Vol. 1 Ed. 3.3 and Vol. 5 Ed. 2.3)
Vol. 5 is scanned (CRC chapter ≈ PDF pages 28–44, Appendix E from 489,
Safety Supervisor ≈195+). Vol. 2 (EtherNet/IP) was not available.
- CRC-S3: width 16, poly 0x080F, no reflection, XorOut 0; CRC of
  `"123456789"` from 0xFFFF = 0x9516 (tested). CRC-S1 0x37, CRC-S2 0x3B,
  CRC-S5 0x5D6DCB (24-bit).
- Base Format actual data CRC (FRS42): PID, `modeByte & 0xE0`, data,
  through CRC-S3. Complement CRC (FRS43): PID, `(modeByte ^ 0xFF) & 0xE0`,
  complemented data.
- Mode byte: bit 7 Run_Idle, 6 TBD_2, 5 TBD, 4 N_Run_Idle, 3 TBD_2 copy,
  2 N_TBD, 1:0 Ping_Count.
- PID: the 32-bit LE connection ID per connection and direction from
  `Forward_Open`/`SafetyOpen` (Vol. 1 Tables 3-5.16/17). Never sent in the
  frame; only seeds the CRC so a wrong sender fails it.
- Message Router (Vol. 1 Tables 2-4.1/2): request = service, path size
  in 16-bit words, padded EPATH, data; response = service|0x80, 0,
  general status, additional status size (words), data.
  Get_Attribute_Single 0x0E, Set_Attribute_Single 0x10.
- Symbolic segment: 0x91, length, ASCII, a pad byte if odd. `"tag1"` =
  `91 04 74 61 67 31` (Table 3-5.15; our encoder matches).
- General status: 0x00 ok, 0x04 path segment error, 0x05 destination
  unknown, 0x08 service not supported, 0x0E not settable, 0x13 not
  enough data, 0x15 too much data.
- Types: BOOL C1, SINT C2, INT C3, DINT C4, LINT C5, USINT C6, UINT C7,
  UDINT C8, ULINT C9, REAL CA, LREAL CB; A2 is our "opaque struct".
- CIP Safety has no broadcast discovery: `Propose_TUNID` (0x56) /
  `Apply_TUNID` commission a device already addressed and in
  `Waiting_for_TUNID`. Generic discovery is EtherNet/IP `List_Identity`
  (general knowledge, Vol. 2, not verified).

### iNetTransport on the STM32L432KC
- An L432 board has one network interface, the W5500 or an ESP-AT module,
  never both. The bring-up firmware's build refuses both. A design that
  needs both goes on a bigger STM32.
- The L432 is most likely the node that sends data out (a transmitter:
  MQTT, `xHttpClient`), not the one that receives or serves. Size new
  features for that. `xHttpServer` is for bigger boards (or
  `maxClients = 1` on an L432).
- RAM: 64 KB. The bring-up firmware with one interface takes about 48.5 KB
  (34 KB of it the FreeRTOS heap); with both it was 54 KB.

### iNetTransport on the STM32F207 (and ESP32)
- The F207 has its own Ethernet MAC and runs lwIP (CubeMX). The same
  interfaces, web server and clients run on it through
  `sockets/SocketNetDevice` built with `INET_SOCKETS_LWIP`, with
  `hw/lwip/LwipNetif` reading the netif. The PLC tag database
  (`PLCTransport`) and its web API live on this board.
- `NetSockets.h` undoes lwIP's `LWIP_COMPAT_SOCKETS` macros (`connect`,
  `poll`, `close`...), which otherwise rewrite our methods of those names.
- `Lwip_test` builds lwIP 2.2.1 for the PC (its Unix port, loopback netif)
  when iNetTransport is configured with `-DLWIP_DIR=<lwIP source>`
  (`git clone --branch STABLE-2_2_1_RELEASE https://github.com/lwip-tcpip/lwip`).
- HTTPS (`tls/MbedTlsServer`, mbedTLS 3.6, `-DMBEDTLS_DIR`) and logins
  (`http/WebAuth`, PBKDF2 via `tls/WebPassword`). PLC tag writes need an
  operator's session, the CSRF token and an allow-list entry
  (`PlcTagWebApi::Config`). Two locks matter: the TLS server's and
  WebAuth's (`TlsFreeRtosLock`). mbedTLS's ticket code gets the raw
  generator, because it already runs under the TLS lock.
- `Tls_test` needs curl and openssl; `PlcWebSecure_test --serve 120` plus
  `NODE_PATH=$(npm root -g) node PLCTransport/web/test/ui_test.cjs` drives
  the built-in page in headless Chromium.
- The UI's files: `storage/` has `SpiNorFlash` (blocking, on an
  `iBlockTransport`), `LittleFsNor` (LittleFS v2.9, `-DLITTLEFS_DIR`,
  `LFS_NO_MALLOC`), and `HttpLittleFsFiles`/`HttpFatFsFiles` (FatFs comes
  from the CubeMX project; `-DFATFS_DIR` only builds the host test,
  `git clone https://github.com/STMicroelectronics/stm32_mw_fatfs`).
  `http/HttpFileAdmin` uploads in 1 KB pieces into a hidden `.part`,
  then commits. A file open for reading is never replaced or removed:
  stores report `busy()`, and `hold()` keeps new readers out while
  HttpFileAdmin waits (`Config::sleep`). Neither file system protects a
  file that is only being read. One lock interface, `inc/iLock.h`, which
  `WebAuth::Lock` and `MbedTlsServer::Lock` now alias.
  `StorageWeb_test --serve 120` plus `storage/test/files_ui_test.cjs`
  drives `/files.html` in Chromium.

## Debugging on hardware

The owner captures with a Saleae logic analyser and sends the CSV
exports; `tools/saleae/saleae_log.py` turns them into a timeline.

- `itransport/inc/DebugLog.h`: text lines `<seq> <ms> <tag> <what>
  [<value>...]` on any `iTransport` UART, and debug pins through the
  port. A log call formats on the stack (no printf), copies into a ring
  (`ITRANSPORT_DEBUG_RING`, 1 KB) under the port's lock and returns, so
  interrupts may log; `dbg::poll()` (idle loop or a low priority task)
  hands the ring to the UART, two 128 byte buffers in turn. A full ring
  drops lines and counts them; the seq gap shows it.
- `ITRANSPORT_DEBUG` (CMake cache variable, PUBLIC on sensor_transport):
  0 off (every macro empty, arguments not evaluated), 1 faults
  (`DBG_FAULT`, `DBG_PIN`/`DBG_PULSE`), 2 + events (`DBG_EVENT`: state
  changes, configuration), 3 + per-transfer trace (`DBG_TRACE`). Define
  it the same in the libraries and the application (in CubeIDE, the
  iTransport library project and the application): `DualChannelLink`'s
  layout and the bus hooks depend on it. `dbg::begin()` pulls in about
  1.3 KB of RAM even at 0; guard it with `#if ITRANSPORT_DEBUG`.
- Hooks so far: `SensorStateMachine` (every driver: `st <from> <to>`,
  `fail`, `bus-err`, `issue-timeout`, `land-timeout`; tag "ssm" until
  `setDebugTag()`), `BusTransport` ("bus": start/done/mutex-busy/in-use/
  not-issued at 3, xfer-fail/irq-idle/irq-unknown/no-slot), ublox_gps
  and mtk3339 (cfg-send/ack/nak/reject/noanswer/done, module-start,
  antenna, rx-overflow, silent; every sentence at 3), `DualChannelLink`
  ("dcl": every fault, and `partner <safe> <healthy> <loopback>
  <hears-us>` on change). `tools/saleae/saleae_log.py --faults` knows
  the fault names; add new ones there.
- Pins (`dbg::DbgPin`): 0 pulsed in the bus completion interrupt, 1 high
  while a bus transfer is in flight (one pin for every bus), 2 pulsed
  on a state change, 3 pulsed on a fault; 4 and up the application's.
- STM32: `hw/stm32/inc/Stm32DebugPort.h` (HAL_GetTick, PRIMASK lock,
  BSRR pins). On the Nucleo-L432KC, USART2 TX is PA2, which is also the
  ST-LINK virtual COM port (from the Nucleo-32 manual, not checked
  here): log there at 921600 8N1 and clip the analyser on PA2. Pins:
  any four free GPIOs, push-pull, very high speed, chosen in CubeMX.
  Sample at 10x the baud rate or more (general knowledge).
- Saleae: Async Serial analyser on the TX channel (921600, 8 bits, no
  parity, 1 stop, LSB first), display radix hex, export the table as
  CSV; export the digital channels as CSV for the pins; then
  `python3 tools/saleae/saleae_log.py --serial s.csv --digital d.csv
  --pins "0=bus-irq,1=transfer,2=state,3=fault"`. The CSV formats it
  reads are from memory of Saleae's documentation: check them against
  the first real export.
- Tests: `itransport/test/debug_log_test.cpp` (level 3, with
  `debug_log_off.cpp` at 0) and `tools/saleae/test_saleae_log.py`
  (made-up exports).

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
cmake -S safeTransport -B build -DSENSOR_FW_HARDWARE=HOST \
      -DITRANSPORT_BUILD_WIRINGPI=OFF -DSENSOR_FW_BUILD_TESTS=ON   # dual_channel_link_test
cmake -S PLCTransport -B build -DSENSOR_FW_BUILD_TESTS=ON \
      -DMBEDTLS_DIR=<mbedtls-3.6.2 source>                         # plc_tags, plc_web, 3 tests
cmake -S iDisplay -B build -DSENSOR_FW_HARDWARE=HOST \
      -DITRANSPORT_BUILD_WIRINGPI=OFF -DSENSOR_FW_BUILD_TESTS=ON   # ssd1306_test, gui_test
cmake -S iRadio -B build -DSENSOR_FW_HARDWARE=HOST \
      -DITRANSPORT_BUILD_WIRINGPI=OFF -DSENSOR_FW_BUILD_TESTS=ON   # davis, rfm95, aes_cmac, lorawan, meshcore tests
```

Last known results: isensor 25 tests (its 15 drivers plus nmea_parser_test, plus itransport's
9), iTransport 9, iNetTransport 25 without the optional source
trees (27 before the two Pico tests, with LWIP_DIR, MBEDTLS_DIR,
LITTLEFS_DIR and FATFS_DIR),
PLCTransport 3 (with MBEDTLS_DIR), safeTransport 10
(dual_channel_link_test and itransport's 9),
iDisplay 12 (ssd1306_test,
hd44780_test, gui_test and itransport's 9), iRadio 19 (davis_test,
davis_rfm69_test, xdavis_rfm69_test, aes_cmac_test, rfm95_test,
lorawan_frame_test, lorawan_mac_test, meshcore_crypto_test,
meshcore_packet_test, meshcore_node_test and itransport's 9), all passing,
at ITRANSPORT_DEBUG 0 and 3. `python3 tools/saleae/test_saleae_log.py`: 5. Each test file also
has a one-line `g++` build command in its header.

`SENSOR_FW_HARDWARE` is `STM32` (default; needs `CMSIS_RTOS_INCLUDE_DIR`,
`STM32_HAL_INCLUDE_DIR`, `STM32_PROJECT_INCLUDE_DIR`), `ARDUINO`
(`ARDUINO_CORE_INCLUDE_DIR`), `RP2040` (from a Pico SDK project, after
`pico_sdk_init()`; builds `PicoTransportLibrary`) or `HOST`. `isensor`, `iNetTransport` and
`safeTransport` find itransport through `ITRANSPORT_DIR` (default
`../iTransport/itransport`). PLCTransport options:
`PLC_MUTEX_USE_CMSIS_RTOS2` (no `std::mutex` on the STM32 toolchain),
`FREERTOS_INCLUDE_DIRS`, `LWIP_INCLUDE_DIRS` for `cip_tag_server`.

For ARM checks, an xPack `arm-none-eabi-gcc` can be downloaded from
GitHub releases; compile with the CubeIDE flags above plus
`-mcpu=cortex-m4 -mfpu=fpv4-sp-d16 -mfloat-abi=hard -mthumb`.

For RP2040 checks, clone the Pico SDK (`git clone --depth 1 --branch
2.1.1 https://github.com/raspberrypi/pico-sdk`; no submodules needed)
and build a small project that includes `pico_sdk_import.cmake`, calls
`pico_sdk_init()`, sets `SENSOR_FW_HARDWARE=RP2040` and
`ITRANSPORT_BUILD_WIRINGPI=OFF`, adds `isensor/` (which adds
itransport), and links `PicoTransportLibrary` and the sensor targets:
configure with `-DPICO_SDK_PATH=... -DPICO_BOARD=pico
-DPICO_PLATFORM=rp2040` and the xPack toolchain on `PATH`. For the
RP2350: `-DPICO_BOARD=pico2 -DPICO_PLATFORM=rp2350-arm-s`, or
`-DPICO_PLATFORM=rp2350-riscv -DPICO_GCC_TRIPLE=riscv-none-elf` with the
xPack `riscv-none-elf-gcc` 14 on `PATH`. RP2350 builds fetch and build
picotool from source the first time (`PICOTOOL_FETCH_FROM_GIT_PATH`
keeps it for reuse).

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
5. Removed `archive/` and the `STM32F207ZG_SafeRelay` CubeIDE project,
   a blank CubeIDE template to be recreated in its own repository (both
   are still in git history: the old main.cpp, main_linux.cpp,
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
17. MQTT 3.1.1 in iNetTransport: `MqttClient` (pure logic, QoS 0/1) and
    `xMqttClient` (its own thread over an `xClient`, on either interface).
18. HTTP/1.1 server in iNetTransport: `HttpLexer` cuts the stream into
    tokens on a FreeRTOS queue, `HttpConnection` (a state machine) builds
    requests and routes them; `xHttpServer`'s daemon creates a thread per
    client.
19. HTTP client for the L432's role as a transmitter: `HttpLexer`'s
    Response mode (chunked, to-close, 1xx), `HttpResponseReader`, and
    `xHttpClient` (caller's thread, no allocation, keep-alive).
20. The web server on lwIP (F207) as well as the W5500/ESP: `SocketNetDevice`
    on BSD sockets, tested over the PC's sockets and over lwIP itself.
    The PLC tags as read-only JSON (`PlcTagWebApi`), the UI served from
    files (`HttpStaticFiles`, with a built-in fallback page).
21. HTTPS and logins on the F207: `MbedTlsServer` behind `iTls.h`,
    `WebAuth` (sessions, roles, CSRF, lockout), PBKDF2 passwords, and
    allow-listed PLC tag writes with an audit log.
22. The UI on SPI flash (LittleFS) or an SD card (FatFs), uploaded from
    `/files.html` by an admin: `storage/`, `HttpFileStore`,
    `HttpFileAdmin`, `iLock`.
23. Pico (RP2040) and Pico 2 (RP2350) I2C, SPI and UART transports.
24. The sensor_fw design record merged into this file. The two-MCU
    safety relay: one loopback UART and one heartbeat UART per MCU,
    `DualChannelLink`, replacing the digital output between them that
    meant "my loopback is complete".
25. NMEA parser and u-blox GNSS driver (`isensor/ublox_gps`), configured
    over UBX (CFG-MSG/CFG-RATE or CFG-VALSET).
26. MediaTek MT3339 GNSS driver (`isensor/mtk3339`, PMTK commands); the
    NMEA parser and a shared `ByteRing` moved to `isensor/nmea`.
27. Debug log and pins for hardware testing with a Saleae (`DebugLog.h`,
    `Stm32DebugPort.h`, `tools/saleae/saleae_log.py`), hooked into
    SensorStateMachine, BusTransport, the GNSS drivers and
    DualChannelLink.
28. The RFM95 (SX1276) LoRa radio driver in iRadio, with a simulated
    air of SX1276s, and AES-128/AES-CMAC for the LoRaWAN MAC to come
    (US915, The Things Network, written in-house).
29. The LoRaWAN 1.0.4 Class A MAC (`lorawan::Mac`, `RegionUS915`,
    `LoRaWanFrame`, `xLoRaWanMac`), tested against a simulated TTN
    gateway and network server.
30. A MeshCore node on the RFM95 (`iRadio/meshcore`): group channels and
    signed adverts, flood sending with MeshCore's budget and
    listen-before-talk; SHA-256/HMAC and AES decryption in
    `iRadio/crypto`, Ed25519 from vendored Monocypher.

## Open items

- Web security: not built yet are a captive portal (Wi-Fi setup on an
  ESP) and users changed at run time (they're compiled in). HTTPS has not
  run on an F207: the handshake time there is an estimate.
- Storage: `SpiNorFlash` and LittleFS have run only against
  `SimSpiNor`, and FatFs only on a RAM disk. No real chip, card, SDIO or
  CubeMX FATFS project yet. `SpiNorFlash` uses single SPI at 4 KB erase
  granularity (no QSPI or memory mapping). FatFs's commit isn't atomic
  (it deletes, then renames). A constantly-read file can make a commit
  wait up to `busyWaitMs`.
- `SocketNetDevice` has not run on an F207 or an ESP32: compiled for
  Cortex-M3 against lwIP 2.2.1 with CubeMX-like options, and run over lwIP
  on a PC. No Xtensa toolchain was used for the ESP32.
- `PlcTagRegistry`'s lock doesn't cover the control program's own writes:
  64-bit values and structs can be read half-updated.

- None of the drivers or libraries has run on hardware. The MMC56x3,
  LSM303DLHC and HMC6352 sequences come from datasheets only.
- `ublox_gps`: not run against a receiver; UBX numbers not checked
  against u-blox's own documents. No baud rate change (CFG-PRT /
  CFG-UART1-BAUDRATE), no UBX-NAV-PVT, no other makers' setup commands
  (Quectel).
- `mtk3339`: not run against a module. No baud rate change (PMTK251),
  no LOCUS logging, standby or firmware query (PMTK605).
- The STM32CubeIDE projects were checked with arm-none-eabi-gcc using
  their `.cproject` settings, but have not been opened in CubeIDE.
- CIP Safety, highest priority (the owner is getting official ODVA
  editions to confirm):
  1. The CRC seed 0x0000 before the PID is an inference, stated nowhere
     in Vol. 1 or 5 (Appendix E uses 0xFFFF only for its self-test). It
     is isolated in `CipSafetyBaseFormatCrc::kAssumedInitialSeed`; look
     there first if a real device disagrees.
  2. FRS45 says "Producer Identifier *byte*" for the time stamp CRC-S1;
     FRS42/43 just say PID. Only the CRC-S3 Base Format (3–250 bytes)
     is implemented.
  3. `CipSafetyCodec` returns false: no frame assembly, Extended Format
     (CRC-S5 split across the timestamp), time coordination/correction.
  4. Whether CIP Safety may run over Wi-Fi, LoRa or MQTT is unknown.
- `safeTransport`:
  - These still include `stm32f4xx_hal.h` instead of `main.h`:
    `Stm32HalCanTransport`, `Stm32_Safe_Relay`, `Stm32L4SafetyRelay`,
    `Stm32SafeUartTransport`, `GpioSafeInput`, `GpioSafeOutput`. Fix
    before using them on the F207 or L4.
  - `CipSafeRelayUartLoopback.cpp` defines `HAL_UART_TxCpltCallback`/
    `RxCpltCallback`, as does itransport's `Stm32UartItCallbacks.cpp`: an
    application can't link both until one dispatches to the other.
  - `UartLoopbackSafe` and `UartLoopbackChannelSafeInput` share state
    between the interrupt and the task with no volatile or critical
    section (`DualChannelLink` uses a ring for this).
  - `DualChannelLink` has not run on chips: no UART at 1 Mbaud, no
    real response time measured, no chip yet chosen. Nothing yet
    reads the chip's UID for the id.
  - Only `DualChannelLink` has a test (the original tests lived in
    `/tmp`). `Stm32HalCanTransport`
    needs a CubeMX project with CAN enabled; `SafeZoneJsonPersistence`
    needs nlohmann/json.
- Deferred, roughly in order: choose the safety relay's chip (a smaller
  STM32, or the F207) and recreate its CubeIDE project in its own
  repository; HSE clock, loopback timeout and serial number; wiring the loopbacks into a `SafeDevice`; the external
  watchdog; `iTransportWifi` and `PlcTagClient`; FreeRTOS and Linux
  `EventQueue`s (ISR-safe push); real EtherNet/IP encapsulation;
  `SafeZone` logic blocks and JSON schema versions; CAN/Ethernet
  `DiscoverSafeDevices()`; OPC-UA; BNO085 reports, `pBNO085`, a ROS2
  altitude publisher from `pBMP280` (RPi5), the KR260 zone controller,
  openSAFETY.
- `iDisplay`: the HD44780 driver covers the I2C backpacks only (not
  direct GPIO, not the 74HC595/SPI side). No inactivity timeout back to home (post `Home` from an
  application timer). FeatherM0_Davis_ISS_Ethernet's screens have not
  been moved onto it.
- `iRadio`: not run against an RFM69 or an ISS. `Stm32RtcClock` not run
  on a chip (compiled against the F407/L432/L476 HAL headers only). Repeater packets are
  delivered on request but never used for timing; no transmit.
- `rfm95`: not run against an RFM95; register values not checked
  against Semtech's datasheet (LoRaMac-node and arduino-LoRa agree).
  No FSK mode, no channel activity detection (CAD), no frequency hopping
  (FHSS), no RFO output (the RFM95W only brings out PA_BOOST).
- LoRaWAN: never joined a real network; only the simulated server.
  Frames checked against lora-packet, timing and rules against
  LoRaMac-node's code, not against the LoRaWAN or RP002 documents
  themselves (not downloaded here). US915 only; no Class B/C, ABP, 1.1,
  rejoin. The session store holds the keys in the clear. FCntDown can
  lag by the downlinks since the last save (a replay of those would be
  accepted after a reset). MAC answers that don't fit wait for the next
  uplink (the application sends one). No RTC-backed GPS time from
  DeviceTimeAns yet (the event carries it).
- MeshCore: never run on air with real MeshCore devices; checked against
  MeshCore's own code built on a PC and a simulated repeater. Not a
  repeater; no direct/anonymous messages, ACKs, path learning, transport
  codes (regions), multipart, trace or CAD. The US preset is remembered,
  not checked (api.meshcore.nz unreachable here). Group packets have no
  sender authentication and a 2 byte MAC (MeshCore's design). Identities
  are Monocypher's 64 byte form (seed | public key), not MeshCore's
  expanded orlp form: a MeshCore device's exported identity can't be
  loaded. The STM32_Static_Lib_Src iRadio projects need crypto/,
  meshcore/ and third_party/ (C sources) added to the sync.
- `iTransport/itransport/REMOVED.txt` is left over from the zip import;
  the files it names are already gone.
- `safeTransport/sensor_fw.zip` and its `*.html` files are old reference
  material.
