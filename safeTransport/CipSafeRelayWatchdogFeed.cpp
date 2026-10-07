// Feeds the IWDG only when UartSafetyLoopbackIsSafe() reports true --
// same "the watchdog is a fail-safe for the safety check ITSELF
// getting stuck" pattern as Stm32L4SafetyRelay used earlier in this
// project: if the UART loopback ever goes unsafe and ISN'T restored
// within the IWDG timeout (~512ms nominal at this project's actual
// Reload=4095/Prescaler=4 configuration -- see the LSI-tolerance
// caveat on that number in Stm32L4SafetyRelay's own header), the MCU
// undergoes a watchdog reset rather than silently continuing to run
// with a safety check that's no longer actually checking anything.
//
// WWDG is deliberately NOT fed here, and has no feed function in this
// file at all -- see the conversation this file came out of for why:
// as configured (Counter=64, the HAL's own minimum valid value),
// WWDG's entire window from init to automatic reset is about one
// counter tick (~136.5us at this project's clock config), which no
// realistic task could ever refresh inside. Writing feed code around
// that would look like a real fix while actually doing nothing --
// the Counter/Window values need correcting in CubeMX first (Counter
// nearer the 7-bit maximum of 127, Window meaningfully below that)
// before a WWDG feed function here would mean anything.
//
// Call FeedIwdgIfSafe() periodically, AFTER PollUartSafetyLoopback()
// has already run for this cycle -- order matters, since this reads
// whatever state that poll just established.

#include "main.h"

extern IWDG_HandleTypeDef hiwdg;

// Declared in CipSafeRelayUartLoopback.cpp.
extern "C" int UartSafetyLoopbackIsSafe();

extern "C" void FeedIwdgIfSafe() {
    if (UartSafetyLoopbackIsSafe()) {
        HAL_IWDG_Refresh(&hiwdg);
    }
    // else: deliberately do nothing -- if this condition persists for
    // the full IWDG timeout, the watchdog reset is the intended,
    // correct outcome, not a failure to handle.
}

// --- Add this to Core/Src/main.c, inside "/* USER CODE BEGIN 2 */",
//     AFTER InitUartSafetyLoopback() (no explicit MX_IWDG_Init() call
//     needed here -- main() already calls it before USER CODE BEGIN 2
//     runs) -- nothing else to add; HAL_IWDG_Init() doesn't need or
//     use an NVIC interrupt, unlike the UARTs. ---
//
// --- And in whichever task already calls PollUartSafetyLoopback(),
//     right after it: ---
//
// PollUartSafetyLoopback(osKernelGetTickCount());
// FeedIwdgIfSafe();
