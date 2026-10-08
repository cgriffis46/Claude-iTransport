#pragma once
// The STM32 RTC as an iClock: a counter of RTC subsecond ticks, from the
// calendar and the subsecond register, that runs on the LSE watch crystal
// (20 ppm), in Stop mode too, whatever clocks the CPU.
//
// Includes the project's own main.h, as the other STM32 classes here do,
// for the family's HAL and CMSIS headers.
#include "main.h"
#include "iClock.h"

// One tick is one step of the RTC's subsecond counter, so
// ticksPerSecond() is PREDIV_S + 1:
//
//   PREDIV_A  PREDIV_S  ticks/s  one tick
//     127       255       256    3.9 ms   (CubeMX's default)
//      15      2047      2048    0.49 ms
//       0     32767     32768    30.5 us  (finest; a little more current)
//
// For the Davis receiver pick one where ticksPerSecond() is a multiple of
// 16 (all three are): its intervals are (41 + id) / 16 s, and then a whole
// number of ticks.
//
// now() reads SSR, then TR, then DR, with interrupts masked for those
// three reads. Reading SSR freezes TR and DR (the shadow registers) until
// DR is read, so the three belong together; BYPSHAD must be 0 (the reset
// value). It is the seconds since 2000-01-01 times ticksPerSecond(), plus
// the ticks into the second, wrapping at 2^32 (24 days at 2048 ticks/s).
// The RTC must be in 24 hour format. After Stop mode, wait for RSF
// (HAL_RTC_WaitForSynchro) before the first read, as for any RTC read.
//
// Setting the time (from SNTP, say) moves now() along with it. Users of
// an iClock that keep schedules (DavisSchedule) notice the jump and start
// over; set it rarely, or with the RTC's shift feature for small changes.
//
// timestamp(): the RTC's timestamp unit latches the calendar and SSR on an
// edge of the RTC_TS pin, in hardware, however late the interrupt runs.
// Wire the radio's DIO0 to that pin (PC13 on most STM32F4/L4 parts; not
// on 32 pin packages), enable it (HAL_RTCEx_SetTimeStamp_IT, rising edge),
// and in HAL_RTCEx_TimeStampEventCallback hand timestamp() to the receiver.
// The latched date has no year: it is taken from the calendar, a year
// back if the stamp is from a later month (a December stamp read in
// January).
class Stm32RtcClock : public iClock {
public:
    // hrtc: the handle MX_RTC_Init() set up. Nothing is read here, so the
    // object can be built before main() runs.
    explicit Stm32RtcClock(RTC_HandleTypeDef* hrtc) : hrtc_(hrtc) {}

    uint32_t ticksPerSecond() const override;
    uint32_t now() override;

    // The time the timestamp unit latched, in now()'s ticks. From
    // HAL_RTCEx_TimeStampEventCallback (it clears the flag).
    uint32_t timestamp();

    // ---- the arithmetic, on its own for tests ----
    // Days from 2000-01-01 to year (0-99, i.e. 2000-2099), month (1-12), day (1-31).
    static uint32_t daysSince2000(uint32_t year, uint32_t month, uint32_t day);
    // (days x 86400 + secondsOfDay) x (predivS + 1) + the ticks into the
    // second (SSR counts down from predivS), modulo 2^32.
    static uint32_t ticks(uint32_t days, uint32_t secondsOfDay, uint32_t ssr, uint32_t predivS);
    // The same from raw RTC_TR / RTC_DR (BCD) and RTC_SSR.
    static uint32_t fromRegisters(uint32_t tr, uint32_t dr, uint32_t ssr, uint32_t predivS);

private:
    uint32_t predivS() const;

    RTC_HandleTypeDef* hrtc_;
    mutable uint32_t predivS_ = 0xFFFFFFFFu;   // read from PRER on first use
};
