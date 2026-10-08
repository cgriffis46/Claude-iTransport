#pragma once
#include <stdint.h>
#include "cmsis_os2.h"
#include "xGuiCore.h"
#include "xButton.h"
#include "iDisplayDevice.h"

namespace idisplay {

// The GUI task (CMSIS-RTOS2). It owns the display: nothing else draws
// on it or flushes it, and the only way in is its event queue.
//
//   - Buttons (xGuiButton) only post key events to the queue, from
//     their pin interrupts.
//   - Other threads post Refresh when there is new data to show, or a
//     screen change (show, push, pop, home), with post().
//   - The task waits on the queue and does nothing else: no timer and
//     no polling. Each event goes to the top screen or the screen stack
//     (xGuiCore), then the screen is drawn into the display's buffer
//     and flushed. Only the pages that changed go out on the bus.
//
// The queue holds one event by default. That is the flow control:
// while an event is waiting, or the task is still drawing the last
// one, the next button press finds the queue full and is dropped, so
// a bouncing or held button cannot pile up presses behind a slow bus.
// The cost is that a Refresh posted with no timeout is dropped too
// while a key is waiting; it is not missed, as the key's redraw shows
// the new data anyway.
//
//   static xssd1306<Stm32HalI2CTransport, 64> oled(param, &hi2c1, SSD1306_I2C_ADDR, i2c1Mutex);
//   static HomeScreen homeScreen;
//   static xGui gui(oled, homeScreen);
//   static xGuiButton upButton(gui, xKey::Up), downButton(gui, xKey::Down), enterButton(gui, xKey::Enter);
//
//   gui.start();                                   // before osKernelStart()
//
//   void HAL_GPIO_EXTI_Callback(uint16_t pin) {    // EXTI on both edges, pull-up, button to ground
//       if (pin == UP_Pin) upButton.onEdge(HAL_GPIO_ReadPin(UP_GPIO_Port, UP_Pin) == GPIO_PIN_RESET);
//       ...
//   }
//
//   gui.post(xGuiEvent::refresh());                // from the sensor thread, when a reading lands
//
// Use the x* variant of the display driver (xssd1306): its sleep()
// gives up the CPU while a transfer is in flight. With the bare one the
// task spins until the flush is done.
//
// Ticks are taken to be milliseconds, the usual 1 kHz tick.
class xGui {
public:
    xGui(iDisplayDevice& display, xScreen& home, uint32_t queueDepth = 1);

    // Creates the queue and the task. False if either could not be
    // created. Call once, before or after osKernelStart().
    bool start(const char* name = "gui", uint32_t stackBytes = 1024,
               osPriority_t priority = osPriorityNormal);

    // Posts ev to the GUI. From an interrupt, timeoutTicks must be 0.
    // False if the queue stayed full (or start() has not run).
    bool post(const xGuiEvent& ev, uint32_t timeoutTicks = 0);

    osMessageQueueId_t queue() const { return queue_; }
    xGuiCore& core() { return core_; }

    // The task body: opens the home screen, draws it, then handles
    // events for ever. start() runs it in its own task.
    void run();

    // One pass of run() after the first draw: waits up to timeoutTicks
    // for an event and handles it. False on timeout. For tests, and for
    // running the GUI from a task of your own.
    bool runOnce(uint32_t timeoutTicks);

    // Opens the home screen and draws it: the start of run().
    void begin();

private:
    static void threadEntry(void* arg);
    void draw();

    iDisplayDevice& display_;
    xGuiCore core_;
    uint32_t queueDepth_;
    osMessageQueueId_t queue_ = nullptr;
    osThreadId_t thread_ = nullptr;
};

// A button wired to an interrupt on both edges. Its only output is a
// key event posted to the GUI's queue, with no wait: if the queue is
// full the press is dropped.
class xGuiButton {
public:
    xGuiButton(xGui& gui, xKey key, uint16_t debounceMs = 50, bool reportReleases = false)
        : gui_(gui), button_(key, debounceMs, reportReleases) {}

    // From the pin interrupt (or a poll), with the button's state now:
    // true when it is pressed. True if an event was posted.
    bool onEdge(bool down);

    xButton& button() { return button_; }

private:
    xGui& gui_;
    xButton button_;
};

} // namespace idisplay
