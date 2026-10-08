#include "Stm32RtcClock.h"

namespace {

inline uint32_t bcd(uint32_t v) { return (v >> 4) * 10u + (v & 0x0Fu); }

bool leap(uint32_t year) { return (year % 4u) == 0; }   // 2000-2099: every fourth year, 2000 included

// RTC_TR: hours (bits 21-16, BCD), minutes (14-8), seconds (6-0).
uint32_t secondsOfDay(uint32_t tr) {
    const uint32_t h = bcd((tr >> 16) & 0x3Fu);
    const uint32_t m = bcd((tr >> 8) & 0x7Fu);
    const uint32_t s = bcd(tr & 0x7Fu);
    return h * 3600u + m * 60u + s;
}

} // namespace

uint32_t Stm32RtcClock::daysSince2000(uint32_t year, uint32_t month, uint32_t day) {
    static const uint16_t kBeforeMonth[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    if (month < 1 || month > 12) month = 1;
    if (day < 1) day = 1;
    uint32_t days = year * 365u + (year + 3u) / 4u;     // leap days in 2000 .. year-1
    days += kBeforeMonth[month - 1];
    if (month > 2 && leap(year)) ++days;
    return days + day - 1u;
}

uint32_t Stm32RtcClock::ticks(uint32_t days, uint32_t sod, uint32_t ssr, uint32_t predivS) {
    const uint32_t perSecond = predivS + 1u;
    // SSR counts down from PREDIV_S to 0 through each second. After a
    // shift it can read above PREDIV_S for a moment: count that as 0.
    const uint32_t into = ssr > predivS ? 0u : predivS - ssr;
    return (days * 86400u + sod) * perSecond + into;    // modulo 2^32, deliberately
}

uint32_t Stm32RtcClock::fromRegisters(uint32_t tr, uint32_t dr, uint32_t ssr, uint32_t predivS) {
    // RTC_DR: year (bits 23-16, BCD), month (12-8), day (5-0).
    const uint32_t year = bcd((dr >> 16) & 0xFFu);
    const uint32_t month = bcd((dr >> 8) & 0x1Fu);
    const uint32_t day = bcd(dr & 0x3Fu);
    return ticks(daysSince2000(year, month, day), secondsOfDay(tr), ssr, predivS);
}

uint32_t Stm32RtcClock::predivS() const {
    if (predivS_ == 0xFFFFFFFFu) predivS_ = hrtc_->Instance->PRER & 0x7FFFu;
    return predivS_;
}

uint32_t Stm32RtcClock::ticksPerSecond() const {
    return predivS() + 1u;
}

uint32_t Stm32RtcClock::now() {
    const uint32_t p = predivS();
    // SSR first: it freezes TR and DR until DR is read. Masked, so an
    // interrupt that reads the clock too cannot unfreeze them half way.
    const uint32_t primask = __get_PRIMASK();
    __disable_irq();
    const uint32_t ssr = hrtc_->Instance->SSR;
    const uint32_t tr = hrtc_->Instance->TR;
    const uint32_t dr = hrtc_->Instance->DR;
    __set_PRIMASK(primask);
    return fromRegisters(tr, dr, ssr, p);
}

uint32_t Stm32RtcClock::timestamp() {
    RTC_TimeTypeDef t = {};
    RTC_DateTypeDef d = {};
    HAL_RTCEx_GetTimeStamp(hrtc_, &t, &d, RTC_FORMAT_BIN);

    // The latched date has no year: the calendar's, or the one before if
    // the stamp's month is later than now's.
    const uint32_t primask = __get_PRIMASK();
    __disable_irq();
    (void)hrtc_->Instance->SSR;             // freeze, then read DR, as in now()
    (void)hrtc_->Instance->TR;
    const uint32_t dr = hrtc_->Instance->DR;
    __set_PRIMASK(primask);
    uint32_t year = bcd((dr >> 16) & 0xFFu);
    const uint32_t monthNow = bcd((dr >> 8) & 0x1Fu);
    if (d.Month > monthNow && year > 0) --year;

    const uint32_t sod = (uint32_t)t.Hours * 3600u + (uint32_t)t.Minutes * 60u + t.Seconds;
    const uint32_t p = t.SecondFraction ? t.SecondFraction : predivS();
    return ticks(daysSince2000(year, d.Month, d.Date), sod, t.SubSeconds, p);
}
