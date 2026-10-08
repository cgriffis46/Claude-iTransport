// The simulated RTC behind stub_hal_rtc/main.h.
#include "main.h"

uint32_t g_primask = 0;
int g_irqDisables = 0;

static uint32_t toBcd(int v) { return (uint32_t)(((v / 10) << 4) | (v % 10)); }
static int fromBcd(uint32_t v) { return (int)((v >> 4) * 10 + (v & 0x0F)); }

void fakeRtcSet(RTC_HandleTypeDef* h, int year, int month, int day, int hour, int minute, int second, uint32_t ssr) {
    h->Instance->TR = (toBcd(hour) << 16) | (toBcd(minute) << 8) | toBcd(second);
    h->Instance->DR = (toBcd(year) << 16) | (1u << 13) | (toBcd(month) << 8) | toBcd(day);   // weekday Monday, never read
    h->Instance->SSR = ssr;
}

void fakeRtcLatch(RTC_HandleTypeDef* h) {
    const uint32_t tr = h->Instance->TR, dr = h->Instance->DR;
    h->tsHours = (uint8_t)fromBcd((tr >> 16) & 0x3F);
    h->tsMinutes = (uint8_t)fromBcd((tr >> 8) & 0x7F);
    h->tsSeconds = (uint8_t)fromBcd(tr & 0x7F);
    h->tsMonth = (uint8_t)fromBcd((dr >> 8) & 0x1F);
    h->tsDate = (uint8_t)fromBcd(dr & 0x3F);
    h->tsSubSeconds = h->Instance->SSR;
}

HAL_StatusTypeDef HAL_RTCEx_GetTimeStamp(RTC_HandleTypeDef* h, RTC_TimeTypeDef* t, RTC_DateTypeDef* d, uint32_t format) {
    (void)format;
    t->Hours = h->tsHours; t->Minutes = h->tsMinutes; t->Seconds = h->tsSeconds;
    t->SubSeconds = h->tsSubSeconds;
    t->SecondFraction = h->Instance->PRER & 0x7FFF;
    d->Month = h->tsMonth; d->Date = h->tsDate; d->Year = 0;    // the timestamp unit keeps no year
    return HAL_OK;
}
