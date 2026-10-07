// Wires UART4 and UART5 into a dual-channel UartLoopbackSafe check for
// the STM32F207ZG_SafeRelay project.
//
// REQUIRES AN EXTERNAL HARDWARE JUMPER on each UART: UART4_TX (PA0)
// to UART4_RX (PC11), and UART5_TX (PC12) to UART5_RX (PD2) -- per
// this project's own .ioc pin assignment. Without that wire, each
// channel will simply never see its own transmission echo back, and
// will correctly, permanently report unsafe (never optimistically
// safe -- see UartLoopbackSafe.h). That's the intended behavior for
// a missing/broken loopback path, not a bug to chase.
//
// BUILD REQUIREMENT, stated rather than assumed: this is a .cpp file
// in a project whose Core/Src is currently pure C. STM32CubeIDE
// supports mixed C/C++ in one project, but adding the FIRST .cpp file
// is usually what triggers it to pull in the C++ runtime/standard
// library for linking -- worth confirming the build actually picks
// this up (check project build settings for a C++ toolchain) rather
// than assuming it's automatic.
//
// NVIC: RESOLVED by enabling UART4_IRQn/UART5_IRQn in CubeMX and
// regenerating -- confirmed directly against the regenerated project,
// not assumed. HAL_UART_MspInit() (Core/Src/stm32f2xx_hal_msp.c) now
// calls HAL_NVIC_SetPriority()/EnableIRQ() for both UARTs itself, and
// runs automatically inside HAL_UART_Init() -- meaning
// MX_UART4_Init()/MX_UART5_Init() (already called from main()) fully
// handle this with no manual NVIC code needed at all. An earlier
// version of this file had its own EnableUartSafetyLoopbackInterrupts()
// doing this by hand specifically because the NVIC wasn't enabled yet
// in the project at the time -- removed now that it's redundant.
//
// CubeMX also auto-generated complete UART4_IRQHandler/
// UART5_IRQHandler functions in stm32f2xx_it.c (each correctly
// calling HAL_UART_IRQHandler). Do NOT also paste handler definitions
// from an earlier version of this file's instructions into that
// file's generic USER CODE BEGIN 0 section -- that would be a
// duplicate definition of the same function name and fail to build.
// Nothing further needs adding to stm32f2xx_it.c at all now.

#include "main.h"
#include "Stm32HalUartTransport.h"
#include "UartLoopbackSafe.h"

extern UART_HandleTypeDef huart4;
extern UART_HandleTypeDef huart5;

namespace {
    Stm32HalUartTransport* g_uart4Transport = nullptr;
    Stm32HalUartTransport* g_uart5Transport = nullptr;
    UartLoopbackSafe*      g_uartLoopback   = nullptr;

    // Known byte pattern transmitted and compared on both channels --
    // TODO, genuinely: replace with this specific device's real
    // serial number (matching the pattern Stm32L4SafetyRelay used
    // elsewhere in this project) once one's assigned, rather than
    // shipping this placeholder pattern as-is.
    const uint8_t kLoopbackPattern[] = {0xA5, 0x5A, 0x3C, 0xC3};
}

// Call once, from main()'s USER CODE BEGIN 2 (after MX_UART4_Init()/
// MX_UART5_Init() have already run, which is also where NVIC gets
// enabled now -- see this file's header comment).
extern "C" void InitUartSafetyLoopback() {
    g_uart4Transport = new Stm32HalUartTransport(&huart4);
    g_uart5Transport = new Stm32HalUartTransport(&huart5);

    // timeoutTicks: tune to this project's actual tick rate and real-
    // world round-trip budget -- 50 is a placeholder, not a verified
    // value for this specific hardware/baud rate combination.
    g_uartLoopback = new UartLoopbackSafe(*g_uart4Transport, *g_uart5Transport,
                                           kLoopbackPattern, sizeof(kLoopbackPattern),
                                           /*timeoutTicks=*/50);
}

// Call periodically from whichever task owns this check -- matches
// UartLoopbackSafe's own established poll() convention. nowTicks
// should be the same tick source/units as timeoutTicks above (e.g.
// osKernelGetTickCount() if using the default 1ms FreeRTOS tick).
extern "C" void PollUartSafetyLoopback(uint32_t nowTicks) {
    if (g_uartLoopback) {
        g_uartLoopback->poll(nowTicks);
    }
}

// Exposed so calling code (a SafeDevice/SafeZone wiring it in, per
// this project's established architecture) can actually use the
// result -- without this, the loopback would run and silently do
// nothing observable.
extern "C" int UartSafetyLoopbackIsSafe() {
    if (!g_uartLoopback) return 0;
    return (g_uartLoopback->GetSafe1State() && g_uartLoopback->GetSafe2State()) ? 1 : 0;
}

// extern "C" so these override the HAL's weak default implementations
// with correctly-matching (unmangled) symbol names -- without
// extern "C" here, C++ name mangling would mean these simply never
// override anything, with no link error to catch the mistake; the
// loopback would silently never see an RX/TX completion at all.
extern "C" void HAL_UART_TxCpltCallback(UART_HandleTypeDef* huart) {
    Stm32HalUartTransport::onTxComplete(huart);
}
extern "C" void HAL_UART_RxCpltCallback(UART_HandleTypeDef* huart) {
    Stm32HalUartTransport::onRxComplete(huart);
}

// --- Nothing to add to stm32f2xx_it.c -- CubeMX's own generated
//     UART4_IRQHandler/UART5_IRQHandler already do the right thing. ---
//
// --- Add this to Core/Src/main.c, inside "/* USER CODE BEGIN 2 */"
//     (runs after MX_UART4_Init()/MX_UART5_Init(), before the
//     scheduler starts) ---
//
// InitUartSafetyLoopback();
//
// --- And wherever a periodic task already exists (or a new one,
//     dedicated to this check) ---
//
// PollUartSafetyLoopback(osKernelGetTickCount());
