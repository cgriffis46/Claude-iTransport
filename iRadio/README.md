# iRadio

Radio transceivers and the protocols spoken over them: a receiver for the Davis
Vantage Pro2 / Vue ISS on an RFM69 (SX1231), a LoRa radio driver for the RFM95W
(SX1276), and on top of it either a LoRaWAN 1.0.4 Class A end device (US915, The Things
Network) or a MeshCore mesh node.

```
rfm69/inc/        RFM69Regs.h: the SX1231 / RFM69 registers used here
davis/            davis_protocol (pure logic, no radio, no RTOS):
                    DavisProtocol   hop tables, intervals, bit order, CRC, packet decode
                    DavisSchedule   which channel to listen on, and until when
                    DavisWeather    one station's latest readings and rain total
                  davis_rfm69<TTransport>: the receiver, a non-blocking state machine
hw/freertos/      xdavis_rfm69: the receiver in its own FreeRTOS task, with a packet
                  queue, a log stream buffer, a command queue and DIO0 wake-up
rfm95/            rfm95<TTransport>: the RFM95W / SX1276 in LoRa mode (header only),
                  xrfm95 (CMSIS-RTOS2), SX1276Regs.h, LoRaPhy.h (FRF, time on air,
                  RSSI and SNR)
crypto/           Aes128 (both ways), AesCmac, Sha256 and HmacSha256 (header only)
lorawan/          LoRaWAN: Mac (Class A, OTAA, MAC commands, ADR), LoRaWanFrame (frames,
                  keys, MIC), Region / RegionUS915, xLoRaWanMac (CMSIS-RTOS2 loop)
meshcore/         MeshCore: Node (channels, adverts, flood sending), MeshPacket,
                  MeshCrypto, MeshIdentity (Ed25519), MeshMessages, xMeshNode
third_party/      Monocypher 4.0.2 (Ed25519; BSD-2 / CC0, vendored unchanged)
test/             host tests, a simulated RFM69 and simulated ISS stations (sim/),
                  simulated SX1276 radios sharing an air (sim/SimSX1276.h), a
                  simulated TTN gateway and network server (sim/SimLoRaWanServer.h),
                  radio ranges and ValidHeader for mesh tests,
                  and single threaded FreeRTOS and CMSIS-RTOS2 stand-ins (stub/)
```

## How it fits together

```
 DIO0 (PayloadReady) ─► onDio0FromISR() ─► task notification ─┐
 postStationActive / postBand / postResync ─► command queue ───┤
                                                                ▼
                         xdavis_rfm69 task: step() ─► davis_rfm69::main()
                           sleeps in ulTaskNotifyTake()  │   ▲
                                                         │   │ DavisSchedule: tune to channel c
                                    SPITransport ◄───────┘   │ until t; a packet re-anchors it
                                         │
                                       RFM69
                                                         │
                       packets() queue ◄─────────────────┤  one DavisPacket per good packet
                       log() stream buffer ◄─────────────┘  one text line per packet
```

- **Queues and stream buffers.**
  - Good packets go out through a FreeRTOS **queue** of `DavisPacket`: the raw bytes,
    station, channel, RSSI, frequency error and receive time. Pass them to `decode()` or
    `DavisWeather`.
  - An optional **stream buffer** carries one text line per packet, for a task that writes
    it to a UART, a file or a socket. Lines that don't fit are left out whole.
  - Commands from other tasks go in through a second queue.
  - Nothing ever blocks the radio task. A full queue drops the packet and counts it.
- **Non-blocking.** Every register access is a state that starts it and a state that
  waits for it, as in the sensor drivers.
  - While it waits for a packet, the state machine calls `sleep()` with how long it may
    wait. `xdavis_rfm69` turns that into `ulTaskNotifyTake()`, which the DIO0 interrupt
    or a command cuts short.
  - Without DIO0 wired, it reads RegIrqFlags2 every 2 ms instead.
  - The SPI bus is taken per transfer, not for a whole receive, so a W5500 on the same bus
    keeps working.
- **Timing.**
  - A station sends every (41 + id)/16 s and moves one channel on each time. Times are
    kept in ticks of the receiver's clock: 1/16 ms of the RTOS tick by default, or the
    STM32 RTC's (see below). At any rate that is a multiple of 16 the interval is a whole
    number of ticks, so a run of missed packets doesn't add rounding error.
  - The receiver tunes 30 ms before a packet is due and waits until 20 ms after. If a
    packet doesn't come, it is counted as missed and the station is expected one
    interval later, one channel on. After 50 misses in a row the station is lost and
    looked for again.
  - To find a station, it sits on one channel for one hop cycle plus one interval
    (about 134 s for id 0), then moves on.
  - With several stations, a station due soon always wins; discovery uses the time in
    between.

## Timing by the STM32 RTC

By default packets are timed by the RTOS tick, which is only as good as the oscillator
the CPU runs on. The schedule re-anchors on every packet, so it needs the clock to be
right to well under 1% over one 2.56 s interval:

- **Uncalibrated RC oscillator.** If the CPU runs from one (an STM32L4's MSI without LSE
  calibration), that isn't guaranteed. In the host test a tick running 1% fast keeps
  **1%** of the packets.
- **Low-power modes.** The tick stops in Stop mode.

`setClock()` gives the receiver a better clock, an `iClock` (itransport): something that
counts ticks at a known rate and can be read from an interrupt.

- **What `Stm32RtcClock` is.** An `iClock` built from the RTC's calendar and subsecond
  counter, on the 32.768 kHz LSE crystal (±20 ppm). It keeps running in Stop mode,
  whatever clocks the CPU.
- **Resolution.** One tick is one step of the subsecond counter: `PREDIV_S + 1` ticks a
  second. CubeMX's default 127/255 gives 3.9 ms ticks, 15/2047 gives 0.49 ms, and 0/32767
  gives 30.5 µs. All three are multiples of 16, so a Davis interval is exactly
  (41 + id) × (ticks a second) / 16 of them. The ISS is almost certainly timed by a watch
  crystal too.
- **Timestamps.** If DIO0 is wired to the **RTC_TS** pin (PC13 on most F4/L4 parts; not
  on 32-pin packages), the RTC's timestamp unit latches the moment each packet ends in
  hardware. `onDio0FromISRAt(rtcClock.timestamp())` passes that on, so interrupt
  latency doesn't matter. On any other pin, `onDio0FromISR()` reads the clock in the
  interrupt.
- **Setting the RTC.** If the RTC is set while running (from SNTP), the receiver sees the
  clock step against the RTOS tick and shifts the schedule by the step, so no station is
  lost. A jump it can't account for (a step of more than a second back, or more than ten
  minutes forward) makes it start over and find the stations again.

```cpp
#include "Stm32RtcClock.h"

static Stm32RtcClock rtcClock(&hrtc);            // MX_RTC_Init(): LSE, 24 h, PREDIV_A 15, PREDIV_S 2047

void app_init() {
    radio.setClock(&rtcClock);                   // before start()
    radio.start();
    HAL_RTCEx_SetTimeStamp_IT(&hrtc, RTC_TIMESTAMPEDGE_RISING, RTC_TIMESTAMPPIN_DEFAULT);
}

// DIO0 on RTC_TS (PC13): the RTC latched the end of the packet.
void HAL_RTCEx_TimeStampEventCallback(RTC_HandleTypeDef* h) {
    radio.onDio0FromISRAt(rtcClock.timestamp());
}
```

Each packet's `rxTicks` is then its end time in RTC ticks. In the host test, consecutive
packets are exactly 83968 ticks apart at 32768 a second. `rxMs` is the RTOS time the task
saw it.

## Example (STM32, FreeRTOS)

```cpp
#include "Stm32HalSPITransport.h"
#include "xdavis_rfm69.h"
#include "DavisWeather.h"
using namespace DAVIS;

static davis_param_t radioParam() {
    davis_param_t p = davis_default_param();     // US band, transmitter 1
    p.dio0_interrupt = true;                     // DIO0 on an EXTI line, rising edge
    return p;
}
static xdavis_rfm69<Stm32HalSPITransport> radio(radioParam(), &hspi1, RFM_CS_GPIO_Port, RFM_CS_Pin, spi1Mutex);

void app_init() {
    xdavis_config_t cfg;
    cfg.logBytes = 512;                          // the text log; 0 for none
    radio.start("davis", 384, tskIDLE_PRIORITY + 3, cfg);
}

void HAL_GPIO_EXTI_Callback(uint16_t pin) {
    if (pin == RFM_DIO0_Pin) radio.onDio0FromISR();
}

void weatherTask(void*) {                        // the consumer
    DavisWeather iss(false);                     // Vantage Pro2; true for a Vue
    DavisPacket p;
    for (;;) {
        if (xQueueReceive(radio.packets(), &p, portMAX_DELAY) != pdPASS) continue;
        if (p.station == 0) iss.update(p);       // transmitter 1
        // iss.temperatureF, iss.humidity, iss.windSpeedMph, iss.rainInches() ...
    }
}

// From the GUI, say: listen for transmitter 3 too.
radio.postStationActive(2, true);
```

The RFM69 sets bit 7 of a register address to **write**, which is the opposite of most
sensors. `SPITransport` now has `setAddressBit(AddressBit::WriteHigh)`, and `davis_rfm69`
sets it on its own transport. After configuring, it reads the sync word back, so an SPI
link that gets this wrong fails at start-up instead of receiving nothing.

## Porting from FeatherM0_Davis_ISS_Ethernet

The sketch's `DavisRFM69` was DeKay's library (CC-BY-SA, after LowPowerLab's RFM69)
with FreeRTOS added. This is a new implementation, written from the SX1231 datasheet
and the published Davis protocol notes. Only facts are kept: the hop tables, sync word,
bit rate, deviation and the register settings known to work on the air.

| Sketch | iRadio |
|---|---|
| `radio.loop()` plus the interrupt handler reading the FIFO under `SPIBusSemaphore` | `davis_rfm69::main()`, every transfer non-blocking; the bus is taken per transfer |
| `PacketFifo` (8, copied by the ISR) and `decode_packet()` printing to Serial | `packets()` queue of `DavisPacket`; `decode()` / `DavisWeather`; `log()` stream buffer for text |
| `Station stations[8]` with `active`, timing, counters | `davis_param_t::active_stations`, `DavisSchedule::station(id)` for the counters |
| Interval in whole ticks: `(41+id)*1000/16` ms | Exact, in 1/16 ms or in RTC ticks |
| Timed by `xTaskGetTickCount()` and `micros()`; the PCF8523 RTC only for the date | Timed by the STM32 RTC (`setClock()`), DIO0 latched by its timestamp unit |
| Tune in 50 ms early, compare with `xTaskGetTickCount()>` (wrong across the rollover) | Tune 30 ms early, signed differences throughout |
| Discovery: 150 s per channel | One cycle plus an interval per channel; a lost station is looked for first where it was due |
| `delayMicroseconds(POST_RX_WAIT)` in the interrupt | Nothing blocks; AutoRxRestart re-arms RX after the FIFO is read |
| `Serial.print` debugging | Counters in `stats()` and `schedule().station(id)` |

Bugs from the sketch's decode that are fixed in `decode()`:
- **Vue wind direction** was rounded with integer division before scaling.
- **Gust index** was `packet[5] & 0xf0 >> 4`, which is `packet[5] & 0x0F` because of
  operator precedence.
- **Rain** stored a stale value instead of the counter.

## The RFM95 (SX1276) LoRa radio

`rfm95<TTransport>` is the radio only, one request at a time, each request carrying its
own channel and settings (`lora::Config`: frequency, SF 7-12, bandwidth, coding rate,
preamble, CRC, I/Q inversion, power). That is how LoRaWAN drives a radio: a different
channel and data rate for each uplink and each receive window.

```cpp
#include "Stm32HalSPITransport.h"
#include "xrfm95.h"

static xrfm95<Stm32HalSPITransport> radio(rfm95_default_param(), &hspi1, RFM_CS_GPIO_Port, RFM_CS_Pin, spi1Mutex);
// EXTI on DIO0 (TxDone/RxDone), and DIO1 (RxTimeout) if wired:
//   radio.onDio0(HAL_GetTick());   with param.dio0Interrupt = true

void radioTask(void*) {
    lora::Config up = lora::defaultConfig(902300000u);   // US915 channel 0, SF7/125 kHz
    radio.transmit(up, payload, len);
    for (;;) {
        radio.main(osKernelGetTickCount());
        rfm95_event_t e; uint8_t buf[255];
        if (radio.takeEvent(&e, buf, sizeof buf)) { /* tx_done, rx_done, rx_timeout, crc_error, fault */ }
    }
}
```

- `receive(cfg, n)` listens for one packet within `n` symbols (RX single, up to 1023,
  what LoRaWAN's receive windows use); `receive(cfg, 0)` listens until `standby()` or
  the next request (RX continuous).
- Every event carries the time it happened: the clock's ticks with `setClock()` (an
  `iClock` such as `Stm32RtcClock`), else ms. With DIO0 wired the interrupt stamps it,
  so a busy thread doesn't shift LoRaWAN's receive windows.
- Startup checks RegVersion (0x12) and reads back the mode and the sync word. A failed
  transfer, a TX that runs past its time on air plus 200 ms, or an RX single past its
  window plus 100 ms is a `fault` event, and the radio starts again after a second.
- Register values are written whole, never read and modified. The FIFO moves in 32 byte
  pieces (the transports' burst limit).
- Power: PA_BOOST (the RFM95W has no RFO pin out), 2-17 dBm, 20 dBm with the high power
  DAC, capped by `param.maxPowerDbm`; the over-current trip is set to match.
- 500 kHz bandwidth gets the SX1276 errata fix (registers 0x36/0x3A).
- About 1.5 KB of RAM an instance (the TX copy, the RX buffer, two event slots).
- Registers and values from Semtech's LoRaMac-node, cross-checked against arduino-LoRa;
  the time on air is Semtech's formula. Not checked against the datasheet itself.

`Aes128` (encrypt only, which is all LoRaWAN needs) and `AesCmac` (RFC 4493) are table
based and small; they are not hardened against timing or power analysis.

## LoRaWAN (Class A, US915, The Things Network)

`lorawan::Mac` is a LoRaWAN 1.0.4 Class A end device, written here (not LMIC or
LoRaMac-node), over any `lora::iLoRaRadio` (`rfm95` is one). Everything it uses is
injected: the radio, a `Region` (`RegionUS915`), and an `iSessionStore` you write for
wherever the session should live (flash, EEPROM, RTC backup registers, a LittleFS file).

```cpp
#include "rfm95.h"
#include "RegionUS915.h"
#include "LoRaWanMac.h"
#include "xLoRaWanMac.h"

static rfm95<Stm32HalSPITransport> radio(radioParam, &hspi1, RFM_CS_GPIO_Port, RFM_CS_Pin, spi1Mutex);
static lorawan::RegionUS915 region(2);           // TTN's sub-band: channels 8-15 and 65
static FlashStore store;                          // your iSessionStore (91 bytes)
static lorawan::Mac mac(radio, region, store, param);   // DevEUI, JoinEUI, AppKey from TTN's console
static lorawan::xLoRaWanMac loop(mac);

// EXTI on DIO0 (and DIO1): radio.onDio0(HAL_GetTick()); loop.wakeFromIsr();

void lorawanTask(void*) {
    mac.begin(osKernelGetTickCount());            // loads the session
    if (!mac.joined()) mac.join();
    for (;;) {
        loop.step(osKernelGetTickCount());        // sleeps until the next window or timer
        lorawan::MacEvent e; uint8_t buf[242];
        while (mac.takeEvent(&e, buf, sizeof buf)) { /* joined, tx_done, downlink, ... */ }
        if (mac.ready() && mac.joined() && haveData) mac.send(1, data, len, false);
    }
}
```

- **Join** (OTAA): DevNonce is a counter, saved *before* each join request goes out; if
  the save fails, nothing is sent. A join accept is checked (MIC, and a JoinNonce
  higher than the last one) and gives the session keys, the DevAddr, the receive window
  settings and the CFList's channel mask. Retries alternate eight 125 kHz tries at DR0
  with one 500 kHz try at DR4, under RP002's backoff (1% of the time in the first hour,
  0.1% to hour 11, 0.01% after).
- **Uplinks**: unconfirmed or confirmed (repeated after RX2 plus 1-3 s until an ACK, as
  many times as `confirmedTries` or the network's NbTrans), on a fresh channel each time,
  every enabled channel used once a round. FCntUp is 32 bits, saved ahead in steps of
  `saveEvery`, so a reset never reuses one.
- **Downlinks** in RX1 or RX2, timed from the TxDone interrupt's stamp: each window opens
  early and listens long enough for `rxErrorMs` of clock error either way (LoRaMac-node's
  formula). FCntDown is rebuilt to 32 bits and must count up; replays, bad MICs and
  other DevAddrs are dropped. A confirmed downlink sets ACK on the next uplink.
- **MAC commands**: LinkADRReq (blocks of them, all US915 channel mask modes), DutyCycleReq,
  RXParamSetupReq and RXTimingSetupReq (answers repeated until a downlink arrives),
  DevStatusReq, LinkCheckReq/Ans and DeviceTimeReq/Ans (`requestLinkCheck()`,
  `requestDeviceTime()`; the time refers to the end of the uplink that asked).
  NewChannelReq, DlChannelReq and TxParamSetupReq are skipped (not US915), as
  LoRaMac-node does.
- **ADR**: the network sets data rate, power and NbTrans; after 64 uplinks without a
  downlink the device asks (ADRACKReq), after 96 it goes back to full power, and every 32
  more one data rate lower, then the default channels.
- RAM: `Mac` 1.4 KB, `RegionUS915` 40 bytes (plus the radio's 1.5 KB). Code about 14 KB
  at -O2 on a Cortex-M4.
- Not here: Class B and C, ABP, LoRaWAN 1.1, regions other than US915 (the `Region`
  interface is where they go), FSK. TTN's fair use policy (30 s of airtime a day) is the
  application's to keep.

## MeshCore

[MeshCore](https://github.com/meshcore-dev/MeshCore) is a LoRa mesh: nodes flood packets
through repeaters, group channels are encrypted with a shared key, and each node has an
Ed25519 identity it announces in signed adverts. `meshcore::Node` is a node for it on any
`lora::iLoRaRadio`, written here from MeshCore's documents and source (protocol v1, as in
firmware v1.12+). A device runs either this or LoRaWAN, not both.

```cpp
#include "rfm95.h"
#include "MeshNode.h"
#include "xMeshNode.h"

rfm95_param_t rp = rfm95_default_param();
rp.syncWord = 0x12;                        // MeshCore's sync word (LoRaWAN's is 0x34)
rp.dio0Interrupt = true;
static rfm95<Stm32HalSPITransport> radio(rp, &hspi1, RFM_CS_GPIO_Port, RFM_CS_Pin, spi1Mutex);
static meshcore::LocalIdentity id;         // id.fromSeed(seed): 32 random bytes, kept in flash
static meshcore::Node node(radio, id, meshcore::defaultNodeParam());   // US: 910.525 MHz SF7 62.5k CR5
static meshcore::xMeshNode loop(node);

// EXTI on DIO0: radio.onDio0(HAL_GetTick()); loop.wakeFromIsr();

void meshTask(void*) {                     // give it a 4 KB stack (Ed25519 verify)
    int8_t pub = node.addChannel(meshcore::kPublicChannelKey, 16);
    int8_t mine = node.addChannel(myKey, 16);
    node.begin(osKernelGetTickCount());
    node.sendAdvert(unixTime, advert);     // type, name, location
    for (;;) {
        loop.step(osKernelGetTickCount());
        meshcore::NodeEvent e; uint8_t buf[184];
        while (node.takeEvent(&e, buf, sizeof buf)) { /* advert, group_text, group_data, sent */ }
        // node.sendGroupData(mine, 0xFF00, reading, n);   or sendGroupText(pub, unixTime, "name", "text")
    }
}
```

- **Receiving**: adverts are verified (Ed25519) and reported with the sender's key, name,
  type, location and hop count; group text and data are reported for the channels the node
  has keys for (up to 4). Duplicates (the last 64 packet hashes) and malformed packets are
  dropped; other payload types (direct messages, paths, ACKs) are counted and ignored.
- **Sending**: adverts (flood or zero hop) and group packets go out by flood for repeaters to
  carry, through a queue of 3, under MeshCore's rules: an airtime budget (50% of each hour by
  default) and listen-before-talk (wait 120-480 ms while a packet is arriving, for up to
  4 s). The node's own packets go into its duplicate table, so a repeater's copy is dropped.
- **Group data** suits sensor readings: a 16 bit type (FF00-FFFF are free for development;
  others are allocated in MeshCore's `docs/number_allocations.md`) and up to 165 bytes.
- **Security, MeshCore's design**: channel packets are AES-128 in ECB mode with a 2 byte
  HMAC tag and no sender signature (anyone with the key can write any name); adverts are
  signed. The "Public" channel's key is well known.
- RAM: `Node` 2.7 KB, plus the radio's 1.5 KB. Flash about 23 KB with Ed25519 on a
  Cortex-M4 (-O2).
- Not here yet: repeating, direct messages (ECDH), ACKs, learned paths, regions
  (transport codes), CAD.

## Building and testing

```
cmake -S iRadio -B build -DSENSOR_FW_HARDWARE=HOST -DITRANSPORT_BUILD_WIRINGPI=OFF -DSENSOR_FW_BUILD_TESTS=ON
cmake --build build && ctest --test-dir build
```

There are ten tests: `davis_test`, `davis_rfm69_test`, `xdavis_rfm69_test`,
`aes_cmac_test`, `rfm95_test`, `lorawan_frame_test`, `lorawan_mac_test`,
`meshcore_crypto_test`, `meshcore_packet_test` and `meshcore_node_test`.

- **`stm32_rtc_clock_test`** (in itransport) runs the real `Stm32RtcClock.cpp` against a
  simulated RTC. It covers the calendar arithmetic, counting across seconds, midnight,
  leap days and new year, and the year the timestamp unit leaves out. `Stm32RtcClock`
  also compiles against ST's own HAL headers for the F407, L432 and L476.
- **`davis_test`** covers:
  - the CRC (the standard check value), bit reversal and hop frequencies;
  - decoding every message type, including missing sensors;
  - the schedule against an ideal receiver: discovery within a cycle, 10 minutes without
    a miss, outages, the very next packet after 45 misses, loss after 50, two stations,
    inactive stations, the EU band, and the ms rollover;
  - the schedule on RTC clocks at 32768, 2048 and 256 ticks a second;
  - a clock set an hour back without warning (it starts over) and with `shift()`
    (nothing missed);
  - rain accumulation across the counter wrap.
- **`davis_rfm69_test`** runs the driver against a simulated RFM69 at register level and
  simulated ISS stations on the real timing.
  - A packet is on the air 6.7 ms, and the radio must be settled on exactly the right
    frequency and Davis settings before it starts.
  - It covers: polled and DIO0 operation, the real `SPITransport`, decoded values, CRC
    errors, outages and loss, two stations, a configured station that is absent, a full
    ring, no radio, a wrong version, a stuck bus, and the EU band across the rollover.
  - With the CPU clock 1% fast it receives 1% of packets on the tick and 100% on the RTC.
    Hardware timestamps make every interval exact; stamping in an EXTI interrupt keeps
    them within 1 ms. With the RTC set an hour on and then two back mid-run, both steps
    are seen and nothing is missed. 256 ticks/s works, and so does switching to the RTC
    while running.
  - Breaking any of these on purpose makes it fail:
    - the SPI write bit
    - the channel order
    - the bit reversal
    - the sync settings
    - the receive timestamp
    - the tune guard (below 18 ms, with the simulated bus taking about 1 ms a transfer)
    - clock-step detection
    - stamping DIO0 or polled packets from the tick instead of the clock
- **`xdavis_rfm69_test`** runs the task layer over the FreeRTOS stand-in. It checks:
  - the queue and stream buffer contents and the log format;
  - drops when the consumer stops;
  - commands and the wake-up they cause;
  - that DIO0 cuts sleeps short, so receive times land on the exact ms;
  - the RTC timestamp path (`onDio0FromISRAt()`), with intervals exact to the tick.

- **`aes_cmac_test`**: AES-128 against FIPS-197 (Appendix C.1 and B) and AES-CMAC
  against RFC 4493's four examples (0, 16, 40 and 64 bytes), fed whole and in pieces.
  Also checked once against OpenSSL 3.0 on 60 random blocks and 60 random messages.
- **`rfm95_test`** runs the driver against simulated SX1276s sharing an air (frequency,
  SF, bandwidth, LDRO, sync word and I/Q must match; collisions; RX single symbol
  timeouts; DIO0/DIO1). It covers the FRF, time-on-air, RSSI and SNR values, startup
  (wrong version, no chip), power and OCP, packets of 1, 2, 32, 33, 200 and 255 bytes,
  I/Q inversion both ways, mismatched settings, RX timeouts (including over 255
  symbols), CRC errors, interrupt time stamps with a late thread, a stuck TX, a stuck
  bus, full event slots, the real `SPITransport`, the tick rollover, and `xrfm95`, and
  a long packet caught in RX single (received whole, not cut off as stuck).
  Breaking the driver on purpose (I/Q, LDRO, the symbol timeout's top bits, the FIFO
  pieces, power, the FIFO pointer, the errata, time stamps, timeouts, the op queue's
  size) makes it fail.
- **`lorawan_frame_test`**: frames made by the independent `lora-packet` library (join
  requests, join accepts with their session keys, uplinks byte for byte, downlinks
  opened; 1000 random ones matched when written, 10 kept in the test) and its README's
  example; `RegionUS915`'s channels, data rates, power limits, RX1 table, join order and
  every LinkADRReq channel mask mode; the receive window formula against a literal copy
  of LoRaMac-node's (1188 cases); and the test server's AES inverse cipher (FIPS-197).
- **`lorawan_mac_test`**: a device (`Mac` over `rfm95` over a simulated SX1276) and a
  simulated TTN gateway and network server on one air, US915 sub-band 2. The server
  writes its own numbers out again (channels, RX1 frequencies and data rates), so a wrong
  region table shows up as a missed downlink. It covers joins (in RX1 and RX2, retries
  under the backoff, giving up, a replayed JoinNonce), DevNonce saved before sending and
  continuing across a reset, a store that can't save, uplinks and their channels and
  power, confirmed uplinks and retransmission, downlinks (RX1 and RX2, confirmed,
  FPending, FCntDown across 16 bits, replays, bad MICs, other addresses, a full buffer),
  every MAC command, ADR backoff over 130 uplinks, the receive window's edges, a 32768 Hz
  clock across the ms and tick rollovers, and `xLoRaWanMac` sleeping only as long as
  allowed. 42 deliberate breaks of the MAC, region and frames are each caught.

- **`meshcore_crypto_test`**: AES-128 decryption (FIPS-197), SHA-256 (FIPS 180-4, a million
  "a", padding boundaries), HMAC-SHA256 (RFC 4231) and Ed25519 (RFC 8032 tests 1-3,
  reproduced with the orlp code MeshCore uses), and the 00/FF key rule.
- **`meshcore_packet_test`**: packets, adverts and group packets against ones made by
  MeshCore's own code (its Packet.cpp, Utils.cpp, Identity.cpp, lib/ed25519 and the Crypto
  library it uses, built on a PC: 900 random ones matched, 10 kept), plus a channel message,
  a datagram and a repeater's advert MeshCore made; a channel message built here is byte
  for byte MeshCore's. Edges: path lengths, UTF-8 cuts, oversize data, MAC bytes, two
  channels with the same hash byte.
- **`meshcore_node_test`**: nodes on a simulated air with radio ranges, a MeshCore-style
  repeater written in the test, and MeshCore-made packets sent raw: messages through one and
  two repeaters (hop counts, duplicates, 2 byte path hashes), the sync word, forged adverts,
  listen-before-talk (waits, and gives up after `lbtMaxMs`), the airtime budget, the queue,
  dropped events, radio faults while sending and listening, and `xMeshNode`. 38 deliberate
  breaks of the MeshCore code and crypto are each caught.

The Davis tests also build as `-std=gnu++14 -fno-exceptions -fno-rtti` with `-Wpedantic -Wshadow`.
The `iRadio` sources, the transport and an example application compile with
`arm-none-eabi-g++` 13.3 against the FreeRTOS V11.1.0 kernel headers, for Cortex-M4F and
Cortex-M0+.

`xrfm95<Stm32HalSPITransport>` compiles without warnings as C++14 against the
STM32L432's HAL and CMSIS-RTOS2 headers, at `ITRANSPORT_DEBUG` 0 and 3, and `rfm95` for
Cortex-M0+. So do the `lorawan` sources and an application with `xLoRaWanMac` over
`rfm95<Stm32HalSPITransport>`, and the `meshcore` sources, Monocypher (C99) and an
application with `xMeshNode` over `rfm95<Stm32HalSPITransport>`.

**Not yet run** against a real RFM69, ISS or RFM95, nor joined to The Things Network: the
LoRaWAN side has only met the simulated server here, and the MeshCore node only MeshCore's
code on a PC and the simulated repeater.
