/*
 * Host test for Stm32RtcClock: the real source against a simulated RTC
 * (stub_hal_rtc/). Checks the calendar arithmetic, that now() counts up
 * one tick per subsecond step across seconds, midnight, month and year
 * ends and leap days, and the year the timestamp unit leaves out.
 *
 *   g++ -std=c++17 -Wall -Wextra -Istub_hal_rtc -I../inc -I../hw/stm32/inc
 *       stm32_rtc_clock_test.cpp stub_hal_rtc/fake_rtc.cpp ../hw/stm32/src/Stm32RtcClock.cpp -o stm32_rtc_clock_test
 */
#include <cstdio>
#include "Stm32RtcClock.h"

static int g_failures = 0;
static void check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failures;
}

int main() {
    std::printf("Stm32RtcClock arithmetic\n");
    check(Stm32RtcClock::daysSince2000(0, 1, 1) == 0, "2000-01-01 is day 0");
    check(Stm32RtcClock::daysSince2000(0, 3, 1) == 60, "2000-03-01 is day 60 (2000 is a leap year)");
    check(Stm32RtcClock::daysSince2000(1, 1, 1) == 366, "2001-01-01 is day 366");
    check(Stm32RtcClock::daysSince2000(24, 2, 29) + 1 == Stm32RtcClock::daysSince2000(24, 3, 1), "2024-02-29 is the day before 03-01");
    check(Stm32RtcClock::daysSince2000(26, 10, 8) == 9777, "2026-10-08 is day 9777");
    check(Stm32RtcClock::daysSince2000(99, 12, 31) == 36524, "2099-12-31 is day 36524");

    RTC_TypeDef regs = {};
    RTC_HandleTypeDef h = {};
    h.Instance = &regs;
    regs.PRER = (15u << 16) | 2047u;                    // PREDIV_A 15, PREDIV_S 2047: 2048 ticks/s
    Stm32RtcClock clock(&h);

    std::printf("Stm32RtcClock::now() on the simulated RTC\n");
    check(clock.ticksPerSecond() == 2048, "2048 ticks a second from PRER");
    fakeRtcSet(&h, 26, 10, 8, 12, 34, 56, 2047);
    const uint32_t t0 = clock.now();
    check(t0 == (9777u * 86400u + 12u * 3600u + 34u * 60u + 56u) * 2048u, "the start of a second: 0 ticks into it");
    check(g_irqDisables == 1 && g_primask == 0, "read with interrupts masked, the mask put back");
    fakeRtcSet(&h, 26, 10, 8, 12, 34, 56, 0);
    check(clock.now() - t0 == 2047, "SSR 0: the last tick of the second");
    fakeRtcSet(&h, 26, 10, 8, 12, 34, 57, 2047);
    check(clock.now() - t0 == 2048, "the next second follows on");
    fakeRtcSet(&h, 26, 10, 8, 23, 59, 59, 0);
    const uint32_t lastTick = clock.now();
    fakeRtcSet(&h, 26, 10, 9, 0, 0, 0, 2047);
    check(clock.now() - lastTick == 1, "midnight: one tick on");
    fakeRtcSet(&h, 24, 2, 28, 23, 59, 59, 0);
    const uint32_t feb28 = clock.now();
    fakeRtcSet(&h, 24, 2, 29, 0, 0, 0, 2047);
    check(clock.now() - feb28 == 1, "into a leap day");
    fakeRtcSet(&h, 26, 12, 31, 23, 59, 59, 0);
    const uint32_t nye = clock.now();
    fakeRtcSet(&h, 27, 1, 1, 0, 0, 0, 2047);
    check(clock.now() - nye == 1, "new year: one tick on");
    regs.SSR = 3000;                                    // just after a shift: above PREDIV_S
    check(clock.now() == (Stm32RtcClock::daysSince2000(27, 1, 1) * 86400u) * 2048u, "SSR above PREDIV_S counts as the start of the second");

    std::printf("Stm32RtcClock::timestamp()\n");
    fakeRtcSet(&h, 26, 10, 8, 7, 0, 0, 1000);
    const uint32_t when = clock.now();
    fakeRtcLatch(&h);
    fakeRtcSet(&h, 26, 10, 8, 7, 0, 3, 5);              // read 3 s later
    check(clock.timestamp() == when, "the latched time, not the time it is read");
    fakeRtcSet(&h, 26, 12, 31, 23, 59, 59, 100);
    const uint32_t dec = clock.now();
    fakeRtcLatch(&h);
    fakeRtcSet(&h, 27, 1, 1, 0, 0, 1, 2047);            // read in the new year
    check(clock.timestamp() == dec, "a December stamp read in January: last year's");

    std::printf("Stm32RtcClock with CubeMX's default prescalers\n");
    RTC_TypeDef regs2 = {};
    RTC_HandleTypeDef h2 = {};
    h2.Instance = &regs2;
    regs2.PRER = (127u << 16) | 255u;
    Stm32RtcClock coarse(&h2);
    fakeRtcSet(&h2, 26, 10, 8, 0, 0, 1, 128);
    check(coarse.ticksPerSecond() == 256 && coarse.now() % 256 == 127, "256 ticks a second, 3.9 ms each");

    std::printf("%s (%d failed)\n", g_failures ? "FAILED" : "all passed", g_failures);
    return g_failures ? 1 : 0;
}
