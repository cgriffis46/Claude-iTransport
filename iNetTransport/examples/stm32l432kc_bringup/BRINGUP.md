# Hardware bring-up: W5500 and ESP-AT on a NUCLEO-L432KC

This firmware brings the network hardware up one step at a time. At each
step it logs what it found, and when a step fails it says what to check.
Once the hardware is up it starts the test services, and
`iNetTransport/tools/net_bringup.py` drives them from a PC. It needs only the GNU Arm
toolchain and ST's STM32CubeL4 package, not a CubeMX project.

> It compiles and links warning-free for the STM32L432KC in every
> configuration, and the drivers pass their host tests. It has not been run
> on a board yet. That's what this guide is for. If a step behaves
> differently from what's described here, the log line is the thing to
> report.

## 1. What you need

- NUCLEO-L432KC
- A W5500 module (WIZ850io, W5500-Lite or a generic "W5500 Ethernet module")
  and an Ethernet cable to a switch or router running DHCP
- Optionally, an ESP module running ESP-AT firmware: an ESP32 DevKit flashed
  with ESP-AT v2.x (recommended), or an ESP-01 with AT 1.7 or later
- Jumper wires, kept short (under 10 cm) if you can: SPI runs at up to 20 MHz
- A PC on the same network, with Python 3
- `arm-none-eabi-gcc` 10 or later, CMake 3.16 or later, and the STM32CubeL4
  firmware package. CubeIDE and CubeMX download it to
  `~/STM32Cube/Repository/STM32Cube_FW_L4_V1.x.y`. It is also on GitHub as
  STMicroelectronics/STM32CubeL4.

## 2. Wiring

Pin names are the STM32 pins, with the NUCLEO-32 header labels in brackets.

| W5500 module | NUCLEO-L432KC | |
|---|---|---|
| 3V3 | 3V3 | about 130 mA at 100 Mbps; the Nucleo's regulator can supply it |
| GND | GND | |
| SCLK | PA5 [A4] | SPI1 |
| MISO | PA6 [A5] | |
| MOSI | PA7 [A6] | |
| SCSn / CS | PA4 [A3] | |
| INTn | PA1 [A1] | optional: without it the driver polls, with slower receive |
| RSTn | PA3 [A2] | optional, but lets the firmware reset the chip |

| ESP-AT module | NUCLEO-L432KC | |
|---|---|---|
| TX | PA10 [D0] | USART1 RX: TX goes to RX |
| RX | PA9 [D1] | USART1 TX |
| EN (CH_PD) | PA8 [D9] | the firmware holds it low, then releases it |
| 3V3 | separate 3.3 V supply | transmit peaks reach about 500 mA; share GND with the Nucleo |
| GND | GND | |

The ESP-01 also needs GPIO0 and GPIO2 high to boot from flash. Most modules
have pull-ups on them.

**Solder bridges.** On the NUCLEO-L432KC, SB16 and SB18 join PA5/PA6 [A4/A5]
to PB7/PB6 [D4/D5] for Arduino Nano compatibility. The firmware leaves
PB6/PB7 in their reset (analog) state, so this is harmless here. Don't wire
anything else to D4/D5, or open the bridges (see UM1956).

The log goes to the ST-LINK virtual COM port (USART2: PA2/PA15). It needs no
extra wiring. LD3 [D13] blinks once a second while the firmware runs, and
blinks fast after a fatal error.

## 3. Build and flash

```sh
cd iNetTransport/examples/stm32l432kc_bringup
cmake -S . -B build -DSTM32CUBE_L4_DIR=$HOME/STM32Cube/Repository/STM32Cube_FW_L4_V1.18.1
cmake --build build
```

With Wi-Fi too:

```sh
cmake -S . -B build -DSTM32CUBE_L4_DIR=... -DBRINGUP_WIFI=ON -DWIFI_SSID="my-ssid" -DWIFI_PASS="my-pass"
```

Other options: `-DBRINGUP_ETH=OFF`, `-DETH_DHCP=OFF` (the static address is
set in `Core/Inc/config.h`), and `-DESP_BAUD=...`.

Flash it in one of these ways:
- copy `build/inet_bringup.bin` onto the `NODE_L432KC` USB drive;
- `STM32_Programmer_CLI -c port=SWD -w build/inet_bringup.hex -v -rst`;
- `openocd -f interface/stlink.cfg -f target/stm32l4x.cfg -c "program build/inet_bringup.elf verify reset exit"`.

## 4. Open the log

The log is at 115200 8N1 on the ST-LINK's serial port (`/dev/ttyACM0`, or
`COMx` on Windows):

```sh
python3 -m serial.tools.miniterm /dev/ttyACM0 115200
```

Press the board's reset button to see the log from the start. On a good
board it should look roughly like this (your numbers will differ):

```
[    0.012] === iNetTransport bring-up: NUCLEO-L432KC ===
[    0.013] SYSCLK 80 MHz, LSE running (MSI trimmed), reset cause: reset pin
[    0.014] building: Ethernet (W5500) yes, Wi-Fi (ESP-AT) no
[    0.015] [eth] W5500: pulsing RSTn (PA3)
[    0.090] [eth] SPI /256 (312 kHz): VERSIONR 0x04, write/read-back, 2 KB DMA transfer: ok
[    0.101] [eth] SPI /32 (2500 kHz): VERSIONR 0x04, write/read-back, 2 KB DMA transfer: ok
[    0.104] [eth] SPI /8 (10000 kHz): ... ok
[    0.106] [eth] SPI /4 (20000 kHz): ... ok
[    0.107] [eth] running SPI at 20000 kHz
[    0.108] [eth] INT (PA1 [A1]) idles high: ok
[    0.109] [eth] PHY: link up, 100 Mbps, full duplex
[    0.110] [eth] MAC 02:08:DC:xx:xx:xx (from the MCU's unique ID)
[    0.125] [eth] driver ready in 14 ms
[    2.130] [eth] address in 2020 ms
[    2.131] [eth] address 192.168.1.77/24, gateway 192.168.1.1, dns 192.168.1.1 (DHCP)
[    2.140] [eth] services: echo :7, discard :9, chargen :19
[   10.140] [stats] up 10 s, heap free 19000 (lowest 18500)
[   10.142] [stats] eth: link up, 192.168.1.77, spi 2412 xfers, 0 restarts, 3 irqs, rx 0 B, tx 0 B, 0 connections
```

## 5. When a step fails

The firmware stops at the first step that can't work and says why. In order:

| Log says | Look at |
|---|---|
| nothing at all | the serial port and baud rate; LD3 blinking fast means a panic printed before you connected, so reset the board |
| `reads 0x00` at /256 | the W5500 has no 3.3 V, RSTn is held low, or MISO is shorted to GND |
| `reads 0xFF` at /256 | CS isn't reaching SCSn, MISO isn't connected, or the module has no power |
| `VERSIONR is not 0x04` | SPI mode (must be mode 0), or SCK and MOSI swapped |
| `writes don't stick` | MOSI not connected |
| slow speeds pass, a fast one fails | signal integrity: shorten the wires and add a ground wire next to SCK. The firmware runs at the fastest speed that passed, so it works anyway |
| `2 KB DMA transfer` corrupt at every speed | DMA: it should not happen with this firmware's settings, so report it |
| `INT ... is low` | INTn not connected (the driver falls back to polling), or the module has no pull-up and the pin has floated |
| `PHY: no link` | the cable, the switch port, the link LEDs on the jack |
| `still waiting for a DHCP lease` | no DHCP server on that network: build with `-DETH_DHCP=OFF` and set the address in `config.h` |
| `[wifi] the module never answered "AT"` | TX/RX swapped, baud rate (ESP-AT v2 defaults to 115200), EN not high, or the supply sagging (use a separate regulator) |
| `[wifi] join failed` | the SSID or passphrase, the band (ESP modules are 2.4 GHz only), or range |
| `*** PANIC: stack overflow in task 'x'` | raise that thread's `stack_size` in `bringup.cpp` / `services.cpp` |
| `*** PANIC: FreeRTOS heap exhausted` | raise `configTOTAL_HEAP_SIZE` in `FreeRTOSConfig.h` (about 13 KB of RAM is free) |
| `*** PANIC: HardFault at pc=...` | `arm-none-eabi-addr2line -e build/inet_bringup.elf <pc>` gives the line; report it |

Once it's running, the `[stats]` line every 10 s is the health check:
- `restarts` above 0 means the driver lost the chip and started again: SPI
  errors on the W5500, or a reset/timeout on the ESP.
- `irqs` stays at 0 on Ethernet when the INT line isn't working.
- `uart overflows` above 0 means the ESP's bytes are arriving faster than
  the driver thread drains them.

A logic analyzer on CS, SCK, MOSI and MISO, decoded as SPI mode 0, makes the
W5500 easy to read. A VERSIONR read is `00 39 00` out and `04` back on the
fourth byte.

## 6. Test from the PC

```sh
python3 iNetTransport/tools/net_bringup.py 192.168.1.77          # the address from the log
python3 iNetTransport/tools/net_bringup.py 192.168.4.20 --wifi   # ESP-AT: echo only
```

It runs these tests, and exits 0 only if all of them pass:

- connect latency
- echo of 13 message sizes from 1 B to 16 KB, every byte checked
- 100 small round trips (min/median/p95/max)
- 50 connect/echo/close cycles, which shows whether sockets are given back
- 1 MB upload (discard)
- 1 MB download (chargen), with every byte checked against the pattern
- all three at once

Worth recording: the SPI speed the probe chose, the throughput and latency
numbers, and the `[stats]` line after the test. For a sense of scale:
- 20 MHz SPI carries at most 2.5 MB/s before protocol overhead.
- ESP-AT at 115200 baud carries at most about 11 KB/s each way.

## 7. Taking it into your own project

`Core/Src/board.cpp` holds the CubeMX settings you need, written out:

- 80 MHz from MSI and the PLL
- SPI1 in mode 0, with DMA1 channels 2 and 3
- USART1 for the ESP and USART2 for the log
- interrupt priorities of 6, inside FreeRTOS's syscall range
- the HAL timebase on TIM6, leaving SysTick to FreeRTOS

`bringup.cpp` shows how to construct the drivers and interfaces and start
their threads. `iNetTransport/examples/stm32l432kc_w5500/net_app.cpp` is the same
code in its smallest form, for a CubeMX project.
