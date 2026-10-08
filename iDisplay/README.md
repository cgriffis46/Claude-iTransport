# iDisplay

Displays, and a small GUI that runs on them: screens, menus, fields for
editing settings with three buttons, and a CMSIS-RTOS2 task that owns
the display.

```
inc/, src/      display_core
                  iTextSurface    a grid of character cells with a cursor; what screens draw on
                  MonoCanvas      a 1 bit frame buffer (SSD1306 page layout) that is also an iTextSurface
                  Font5x7         ASCII 0x20-0x7E and a degree sign at 0x7F, in 6x8 cells
                  iDisplayDevice  what a display driver provides: text(), requestFlush(), main(), idle(), failed()
ssd1306/        SSD1306 128x64 / 128x32 OLED, over I2C (ssd1306) or 4-wire SPI (ssd1306_spi);
                xssd1306 sleeps with osDelay() while a transfer is in flight
gui/            idisplay_gui, no RTOS
                  xScreen, xNavigator  a screen and how it changes the screen shown
                  xGuiCore             the screen stack and what each event does to it
                  xMenu, xMenuScreen   scrolling menu; items open a screen, run an action, or go back
                  xYesNoField, xChoiceField, xTextField   the fields an edit screen is made of
                  xButton              edge debouncing (safe in an interrupt), long press, auto-repeat
hw/freertos/    xGui (the GUI task), xGuiButton, and xGuiButtonGroup (the timer for
                long presses and auto-repeat), CMSIS-RTOS2
```

## How it fits together

```
 pin interrupt ──► xGuiButton ──┐
 button timer ───► (Held,Repeat)┤
                                ├──► queue (1 slot) ──► xGui task ──► xGuiCore ──► top xScreen
 other threads ──► xGui::post ──┘                         │              (onKey / onRefresh /
   (Refresh, Show, Push, Pop, Home)                        │               show, push, pop, home)
                                                           ▼
                                        render() into the frame buffer, requestFlush(),
                                        display.main() until idle: only changed pages go out
```

- The buttons only post key events to the queue. They never call a
  screen or touch the display.
- The GUI task only acts on what it takes off the queue. It has no timer
  and doesn't poll. To show new readings, post `xGuiEvent::refresh()`
  from the thread that has them.
- The queue holds one event, which provides the flow control. While an
  event is waiting, or the last one is still being drawn, a new press
  finds the queue full and is dropped. `xButton` also debounces each
  edge in time, so contact bounce doesn't reach the queue.
- **Long presses and auto-repeat.** A button can be *plain* (`Pressed` as it goes down),
  *long press* (`Pressed` when let go before the long-press time, or `Held` once it reaches
  it, never both), or *auto-repeat* (`Pressed`, then `Repeat` while held). Holding needs a
  clock and a pin interrupt cannot start a CMSIS-RTOS2 timer, so `xGuiButtonGroup` runs a
  periodic timer (20 ms) that asks each button and posts `Held`/`Repeat` to the same queue.
  A `Repeat` that finds the queue full is dropped, so repeating never runs ahead of drawing.
- The ready-made widgets use them: Up/Down step on `Pressed` and `Repeat`; Enter chooses on
  `Pressed`; holding Enter (`Held`) is back in a menu and cancel in a field. With three
  buttons: Up/Down auto-repeat, Enter long press.
- Screens run in the GUI task. They change screens through the
  `xNavigator` they are handed, which takes effect straight away.
- Drawing goes into RAM only. The driver sends a page only when it has
  changed since it was last sent: it keeps a hash of each page it has
  sent. A screen that is cleared and redrawn the same costs no bus time.

## Example (STM32, CMSIS-RTOS2)

```cpp
#include "Stm32HalI2CTransport.h"
#include "xssd1306.h"
#include "xGui.h"
#include "xMenu.h"
#include "xFields.h"
#include "Font5x7.h"     // kDegreeChar

using namespace idisplay;
using namespace SSD1306;

static xssd1306<Stm32HalI2CTransport, 64> oled(ssd1306_default_param(64), &hi2c1, SSD1306_I2C_ADDR, i2c1Mutex);

struct HomeScreen : xScreen {
    void onKey(xKey k, xKeyAction a, xNavigator& nav) override;
    void render(iTextSurface& s) override {
        s.print("Temp: "); s.print(temperature, 1); s.putChar(kDegreeChar);
        s.printAt(0, 1, "Humidity: "); s.print(humidity, 0); s.putChar('%');
    }
} homeScreen;

struct ActiveScreen : xScreen {            // edits one yes/no setting
    xYesNoField field;
    void onEnter(xNavigator&) override { field.set(stations[stationSel].active); }
    void onKey(xKey k, xKeyAction a, xNavigator& nav) override {
        switch (field.onKey(k, a)) {
        case xFieldResult::Done:      stations[stationSel].active = field.value(); saveStations(); nav.pop(); break;
        case xFieldResult::Cancelled: nav.pop(); break;
        default: break;
        }
    }
    void render(iTextSurface& s) override { s.print("Active: "); field.render(s); }
} activeScreen;

static xMenuScreen mainMenu("Main Menu");
void HomeScreen::onKey(xKey, xKeyAction a, xNavigator& nav) { if (a == xKeyAction::Pressed) nav.push(mainMenu); }

static xGui gui(oled, homeScreen);
static xGuiButton upButton(gui, xKey::Up, xButtonConfig::autoRepeat());      // 500 ms, then every 150 ms
static xGuiButton downButton(gui, xKey::Down, xButtonConfig::autoRepeat());
static xGuiButton enterButton(gui, xKey::Enter, xButtonConfig::longPress()); // hold 600 ms = back
static xGuiButtonGroup buttons;            // the timer behind Held and Repeat

void app_init() {                          // before osKernelStart()
    mainMenu.menu().add("Station active", activeScreen);
    mainMenu.menu().addBack();
    gui.start();
    buttons.add(upButton); buttons.add(downButton); buttons.add(enterButton);
    buttons.start();                       // needs configUSE_TIMERS
}

void HAL_GPIO_EXTI_Callback(uint16_t pin) { // EXTI on both edges, pull-up, button to ground
    if (pin == UP_Pin)    upButton.onEdge(HAL_GPIO_ReadPin(UP_GPIO_Port, UP_Pin) == GPIO_PIN_RESET);
    if (pin == DOWN_Pin)  downButton.onEdge(HAL_GPIO_ReadPin(DOWN_GPIO_Port, DOWN_Pin) == GPIO_PIN_RESET);
    if (pin == ENTER_Pin) enterButton.onEdge(HAL_GPIO_ReadPin(ENTER_GPIO_Port, ENTER_Pin) == GPIO_PIN_RESET);
}

// In the sensor thread, when a reading lands:
gui.post(xGuiEvent::refresh());
```

## Porting from FeatherM0_Davis_ISS_Ethernet

These classes started in that sketch's `.ino`. The screens specific to
the weather station stay in the application. What changed:

| Sketch | iDisplay |
|---|---|
| `xDisplay`, with `init()` / `update()` / `saveDisplay()` | `xScreen`, with `onEnter()` / `render(surface)`; saving happens in `onKey()` when a field returns `Done` |
| Each screen's `init()` reassigns `up/down/enter.button_press_handler` | `xScreen::onKey(key, action, nav)`; buttons don't know about screens |
| `xButtonHandlerTask` calls handlers, which change `TheDisplay->TheMenu` | Buttons only post to the GUI queue; screens change only in the GUI task |
| `xDisplayEvent{DISPLAY_SET, xDisplay*}` | `xGuiEvent::show/push/pop/home(screen)`, or `nav.push(...)` from a screen |
| `SetDefaultDisplay()` | `nav.home()` (or `gui.post(xGuiEvent::home())`) |
| `RefreshTimer` redraw every 5 s | `gui.post(xGuiEvent::refresh())` when there is new data |
| `xMenu` with `String` items, 8 max, `AddMenuItemFunction/Submenu` | `xMenu` with `const char*`, 16 max, `add(text, screen)` / `add(text, action, ctx)` / `addBack()` |
| `YNField`, `Choice`, `TextField` | `xYesNoField`, `xChoiceField`, `xTextField` / `xTextFieldN<N>` |
| `oled.print(...)` on a global `Adafruit_FeatherOLED` | `render(iTextSurface& s)`: `s.print(...)`; `s.graphics()` for pixels |
| One copied ISR per button (`xUp`, `xDown`, `xEnter`) | One `xGuiButton` per pin; the ISR passes it the pin level |
| `BUTTON_HELD` (declared, never sent) | `Held` (long press) and `Repeat` (auto-repeat), from `xGuiButtonGroup`'s timer |

Problems in the sketch's version that this fixes:

- **Queue calls in interrupts.** The button interrupts called `xQueueSend(..., 1000)`, which isn't allowed there. `xGuiButton` posts with a zero timeout, which CMSIS-RTOS2 allows from an interrupt.
- **Releases.** Up and Down counted any edge that wasn't a press as a release.
- **Two tasks changing the same menu.** The button task changed menu state while the display task drew it.
- **Menu scrolling.** `EndLine` never moved, and every item from `FirstLine` on was drawn.
- **Submenus.** Choosing a submenu entry did nothing.
- **Text field bounds.** `TextField` read past `CharList`, let the cursor reach 64 in a 64-byte buffer, and drew only 32 characters.
- **Bus held while drawing.** The I2C semaphore was held for the whole redraw. Now drawing is RAM-only, and the transport takes the bus once per transfer.
- **A stopped display stopped the GUI.** A display that stopped responding left the display task waiting on the bus. Now it fails after the 100 ms bus timeout, the GUI carries on, and the display powers up again by itself.

## Building and testing

```
cmake -S iDisplay -B build -DSENSOR_FW_HARDWARE=HOST -DITRANSPORT_BUILD_WIRINGPI=OFF -DSENSOR_FW_BUILD_TESTS=ON
cmake --build build && ctest --test-dir build
```

There are two host tests:

- **`ssd1306_test`** runs the driver against a simulated SSD1306 RAM in three ways: behind a bare `ISensorTransport`, behind the real `I2CTransport`, and over SPI with a D/C pin. It covers the power-up sequence, sending only changed pages, settings, an unplugged chip and recovery, a stuck bus, a refused bus, 128x32, rotation, the SH1106 offset, the tick rollover, and `xssd1306`'s sleeps.
- **`gui_test`** covers text and canvas drawing, the screen stack, menu scrolling and navigation, the three fields, button debouncing, long press and auto-repeat (including the tick rollover and a
  late tick), `Held`/`Repeat` in menus and fields, and `xGui` and `xGuiButtonGroup` over a simulated
  one-slot queue and timer. That last part includes dropped presses and repeats, refresh, and a failed display.

Not yet run on hardware. The SSD1306 sequence comes from the datasheet.
