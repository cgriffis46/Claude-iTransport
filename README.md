# iTransport

Non-blocking C++ drivers for microcontrollers, built on one idea: **a
sensor driver should know what the chip means, not how you talk to it.**

iTransport is the seam between the two. A driver asks for "read 6 bytes
from register 0x28"; the transport decides whether that is I2C on an
STM32, SPI on a Raspberry Pi Pico, Arduino's `Wire`, or a mock chip in a
unit test on your PC. The same driver code runs on all of them.

Everything is written to be tested on a PC first: every driver and
transport in this repository has host tests that simulate the chip or
the peripheral, and runs under `ctest` with no hardware.

## Why it helps

**One driver, many buses and boards.** Plenty of sensors speak both I2C
and SPI (the BMP280, BME280, LPS35HW...). A driver here is a template over
its transport, so the I2C and SPI versions, on STM32, Pico, Arduino or
Linux, are the same source:

```cpp
// STM32 + FreeRTOS, I2C1
xlps35hw<Stm32HalI2CTransport> baro(param, &hi2c1, LPS35HW_ADDR, i2c1Mutex);

// The same driver in a host test, against a simulated chip
xlps35hw<MockBus> baro(param, chip);
```

**Nothing blocks.** Every transport call only *starts* a transfer and
returns at once; completion comes from the bus interrupt (or DMA). A
driver is a small state machine: one state issues a transfer, the next
waits for it to land, with timeouts on both. A slow sensor never stalls
the rest of the firmware, and a bare-metal superloop can run many
drivers side by side. Under an RTOS, the waiting thread sleeps until
the interrupt wakes it instead of polling.

**Shared buses are handled for you.** Several devices on one I2C or SPI
bus share one mutex, passed to each device's transport. The transport
takes the bus for a transfer and hands it back afterwards, and the
interrupt routes each completion to the device that started it.

**Porting is a few small functions.** A new platform implements only the
raw calls: for I2C, `halMemWrite`, `halMemRead`, `halMasterTransmit`,
`halMasterReceive` and `halIsDeviceReady`; for SPI, `halTransmit`,
`halTransmitReceive` and the chip-select pins. Arbitration, completion,
timeouts and error bookkeeping are already written, in `BusTransport`.
Where an RTOS is used, `FreeRtosTransport<TBus>` adds the CMSIS-RTOS2
mutex and thread-flag wake-up to any bus.

**Failures are first class.** A missing chip, a NACK, a stuck bus or a
transfer that never finishes all end the same way: `lastOpFailed()`,
then the driver's error state, a back-off, and a restart. Readings are
`NAN` until the first good one and go back to `NAN` on failure, so stale
values are never mistaken for fresh ones.

**Testable without hardware.** Because drivers see only an interface,
tests plug in a simulated chip and drive time by hand: absent or wrong
chips, refused and stuck transfers, stalled measurements and tick
counter rollover are all covered. The bus transports themselves are
tested too, over faked HAL calls (STM32) or a simulation of the
RP2040/RP2350 peripherals (Pico).

**Small and predictable.** No heap in the transports, buffers owned by
the caller, C++14 with no exceptions and no RTTI: it builds with the
flags STM32CubeIDE uses.

## The interfaces

| Interface | For | Main calls |
| --- | --- | --- |
| `ISensorTransport` | register and command chips (most sensors) | `writeReg`, `writeRegs`, `readRegs`, `writeBytes`, `readBytes`, `isBusy`, `lastOpFailed`, `checkDevice` |
| `iTransport` | byte streams (UART) | `write`, `setRxSink` (bytes are pushed to a sink from the interrupt) |
| `iTransportOneWire` | 1-Wire (DS18B20), over a UART | reset, bit and byte slots |
| `iBlockTransport` | large SPI blocks by DMA (W5500 Ethernet, SPI flash) | header plus data block under one chip-select |
| `iClock` | a free-running time base (e.g. the STM32 RTC) | `now`, `ticksPerSecond` |

## Platforms

| Platform | Transports |
| --- | --- |
| STM32 (HAL, FreeRTOS / CMSIS-RTOS2) | I2C, SPI, SPI block (DMA), UART, 1-Wire over UART, RTC clock |
| Raspberry Pi Pico / Pico 2 (Pico SDK, RP2040 and RP2350 Arm or RISC-V, no RTOS needed) | I2C (interrupt-driven FIFOs), SPI (DMA), UART (interrupt RX, DMA TX) |
| Arduino | I2C over `Wire` |
| Linux (Raspberry Pi, Kria KR260) | I2C through WiringPi or `/dev/i2c-*` |
| PC | mocks and simulations for the host tests |

## What is built on it

| Folder | Contents |
| --- | --- |
| `iTransport/itransport/` | the interfaces, the bus classes and the platform transports above |
| `isensor/` | sensor drivers: AHT20, BME280, BMP280, DS18B20, HMC6352, HTU21DF, LPS35HW, LSM303DLHC, MMC56x3, MPL3115A2, PMS5003 PM2.5 (over a UART), SHT31, Si7021, and u-blox and MediaTek MT3339 GNSS receivers with an NMEA parser |
| `iDisplay/` | SSD1306 OLED and HD44780 character LCD drivers, and a small GUI (screens, menus, fields, debounced buttons) |
| `iRadio/` | a Davis Instruments weather station (ISS) receiver on an RFM69 radio, an RFM95W / SX1276 LoRa radio driver, a LoRaWAN 1.0.4 Class A end device (US915, The Things Network), and a MeshCore mesh node (group channels, signed adverts) |
| `iNetTransport/` | W5500 Ethernet and ESP-AT Wi-Fi, lwIP sockets, DHCP, DNS, SNTP, MQTT and HTTP clients, an HTTP(S) server with logins, and SPI flash / SD card storage |
| `PLCTransport/` | a PLC tag database served over CIP and as JSON for a web UI |
| `safeTransport/` | dual-channel safety inputs, outputs, devices and zones, and the link between the two MCUs of a safety relay |

STM32CubeIDE static libraries built from this code are in
[cgriffis46/STM32_Static_Lib_Src](https://github.com/cgriffis46/STM32_Static_Lib_Src).

## Writing a driver

A driver derives from `SensorStateMachine<TTransport, State>` and
inherits its transport, so everything after the driver's own parameters
goes straight to the transport's constructor. Its `main(nowMs)` is a
`switch` over states, and every bus access is two states:

```cpp
case read_data:
    issued(this->readRegs(REG_OUT, _buf, 6), wait_data, nowMs);
    break;
case wait_data:
    if (landed(nowMs)) { convert(_buf); enter(done, nowMs); }
    break;
```

`issued` and `landed` carry the timeouts and send the driver to its
error state if the transfer is refused, fails or never finishes. The
`x*` variant of each driver (e.g. `xlps35hw`) sleeps with `osDelay()`
between steps, for running it in its own RTOS thread. `lps35hw`,
`HMC6352`, `mmc56x3` and `lsm303dlhc` are good drivers to copy.

## Debugging on hardware

Build with `-DITRANSPORT_DEBUG=1..3` and every driver logs its state
changes and faults as text lines on a spare UART, and drives debug pins
around bus transfers and interrupts, without blocking. Capture both with
a logic analyser; `tools/saleae/saleae_log.py` turns a Saleae export
into a timeline. See `iTransport/itransport/inc/DebugLog.h` and
[CLAUDE.md](CLAUDE.md).

## Building and testing

Every module builds on its own on a PC:

```sh
cmake -S isensor -B build -DSENSOR_FW_HARDWARE=HOST \
      -DITRANSPORT_BUILD_WIRINGPI=OFF -DSENSOR_FW_BUILD_TESTS=ON
cmake --build build && ctest --test-dir build
```

The same works for `iTransport`, `iNetTransport`, `iDisplay`, `iRadio`,
`safeTransport` and `PLCTransport`. For a target, set
`SENSOR_FW_HARDWARE` to `STM32`, `ARDUINO` or `RP2040` (from a Pico SDK
project). [CLAUDE.md](CLAUDE.md) has the full build options and the
design notes, and [CHANGELOG.md](CHANGELOG.md) the history.

## Status

Everything here is tested on a PC, and the STM32 and Pico code is
compiled with the real vendor toolchains and headers. **None of it has
yet run on real hardware**; the chip sequences come from datasheets.
Treat it as a well-tested starting point, not proven firmware, and
check it on your board.

## Donations

BTC: 3NHkEBTHdWWbvDBu8AXpmV8TFhnJPrqzBe
