# iRadio

Radio transceivers and the protocols spoken over them. First up: a receiver for the
Davis Vantage Pro2 / Vue ISS, on an RFM69 (SX1231).

```
rfm69/inc/        RFM69Regs.h: the SX1231 / RFM69 registers used here
davis/            davis_protocol (pure logic, no radio, no RTOS):
                    DavisProtocol   hop tables, intervals, bit order, CRC, packet decode
                    DavisSchedule   which channel to listen on, and until when
                    DavisWeather    one station's latest readings and rain total
                  davis_rfm69<TTransport>: the receiver, a non-blocking state machine
hw/freertos/      xdavis_rfm69: the receiver in its own FreeRTOS task, with a packet
                  queue, a log stream buffer, a command queue and DIO0 wake-up
test/             host tests, a simulated RFM69 and simulated ISS stations (sim/),
                  and a single threaded FreeRTOS stand-in (stub/)
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
    kept in 1/16 ms, where that interval is a whole number, so a run of missed packets
    doesn't add rounding error.
  - The receiver tunes 30 ms before a packet is due and waits until 20 ms after. If a
    packet doesn't come, it is counted as missed and the station is expected one
    interval later, one channel on. After 50 misses in a row the station is lost and
    looked for again.
  - To find a station, it sits on one channel for one hop cycle plus one interval
    (about 134 s for id 0), then moves on.
  - With several stations, a station due soon always wins; discovery uses the time in
    between.

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
| Interval in whole ticks: `(41+id)*1000/16` ms | Exact, in 1/16 ms |
| Tune in 50 ms early, compare with `xTaskGetTickCount()>` (wrong across the rollover) | Tune 30 ms early, signed differences throughout |
| Discovery: 150 s per channel | One cycle plus an interval per channel; a lost station is looked for first where it was due |
| `delayMicroseconds(POST_RX_WAIT)` in the interrupt | Nothing blocks; AutoRxRestart re-arms RX after the FIFO is read |
| `Serial.print` debugging | Counters in `stats()` and `schedule().station(id)` |

Bugs from the sketch's decode that are fixed in `decode()`:
- **Vue wind direction** was rounded with integer division before scaling.
- **Gust index** was `packet[5] & 0xf0 >> 4`, which is `packet[5] & 0x0F` because of
  operator precedence.
- **Rain** stored a stale value instead of the counter.

## Building and testing

```
cmake -S iRadio -B build -DSENSOR_FW_HARDWARE=HOST -DITRANSPORT_BUILD_WIRINGPI=OFF -DSENSOR_FW_BUILD_TESTS=ON
cmake --build build && ctest --test-dir build
```

There are three tests: `davis_test`, `davis_rfm69_test` and `xdavis_rfm69_test`.

- **`davis_test`** covers:
  - the CRC (the standard check value), bit reversal and hop frequencies;
  - decoding every message type, including missing sensors;
  - the schedule against an ideal receiver: discovery within a cycle, 10 minutes without
    a miss, outages, the very next packet after 45 misses, loss after 50, two stations,
    inactive stations, the EU band, and the ms rollover;
  - rain accumulation across the counter wrap.
- **`davis_rfm69_test`** runs the driver against a simulated RFM69 at register level and
  simulated ISS stations on the real timing.
  - A packet is on the air 6.7 ms, and the radio must be settled on exactly the right
    frequency and Davis settings before it starts.
  - It covers: polled and DIO0 operation, the real `SPITransport`, decoded values, CRC
    errors, outages and loss, two stations, a configured station that is absent, a full
    ring, no radio, a wrong version, a stuck bus, and the EU band across the rollover.
  - Breaking any of these on purpose makes it fail:
    - the SPI write bit
    - the channel order
    - the bit reversal
    - the sync settings
    - the receive timestamp
    - the tune guard (below 18 ms, with the simulated bus taking about 1 ms a transfer)
- **`xdavis_rfm69_test`** runs the task layer over the FreeRTOS stand-in. It checks:
  - the queue and stream buffer contents and the log format;
  - drops when the consumer stops;
  - commands and the wake-up they cause;
  - that DIO0 cuts sleeps short, so receive times land on the exact ms.

All three also build as `-std=gnu++14 -fno-exceptions -fno-rtti` with `-Wpedantic -Wshadow`.
The `iRadio` sources, the transport and an example application compile with
`arm-none-eabi-g++` 13.3 against the FreeRTOS V11.1.0 kernel headers, for Cortex-M4F and
Cortex-M0+.

**Not yet run** against a real RFM69 or ISS.
