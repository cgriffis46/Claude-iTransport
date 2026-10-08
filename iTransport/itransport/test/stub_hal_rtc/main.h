// A small simulation of the STM32 RTC registers and the HAL calls
// Stm32RtcClock makes, standing in for a CubeMX project's main.h, so the
// real Stm32RtcClock.cpp builds and runs on a host. The fields and names
// are the real ones; the fake*() functions are the hardware's side.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { HAL_OK = 0, HAL_ERROR, HAL_BUSY, HAL_TIMEOUT } HAL_StatusTypeDef;

typedef struct {
    volatile uint32_t TR, DR, CR, ISR, PRER, WUTR, CALIBR, ALRMAR, ALRMBR, WPR, SSR;
} RTC_TypeDef;

typedef struct {
    RTC_TypeDef* Instance;
    // ---- simulated timestamp unit, not in the real HAL ----
    uint8_t tsHours, tsMinutes, tsSeconds, tsMonth, tsDate;
    uint32_t tsSubSeconds;
} RTC_HandleTypeDef;

typedef struct {
    uint8_t Hours, Minutes, Seconds, TimeFormat;
    uint32_t SubSeconds, SecondFraction, DayLightSaving, StoreOperation;
} RTC_TimeTypeDef;

typedef struct { uint8_t WeekDay, Month, Date, Year; } RTC_DateTypeDef;

#define RTC_FORMAT_BIN 0x00000000U

HAL_StatusTypeDef HAL_RTCEx_GetTimeStamp(RTC_HandleTypeDef* hrtc, RTC_TimeTypeDef* t, RTC_DateTypeDef* d, uint32_t format);

// CMSIS core: the interrupt mask. The fake counts how often it is set.
extern uint32_t g_primask;
extern int g_irqDisables;
static inline uint32_t __get_PRIMASK(void) { return g_primask; }
static inline void __set_PRIMASK(uint32_t v) { g_primask = v; }
static inline void __disable_irq(void) { g_primask = 1; ++g_irqDisables; }

// ---- the hardware's side, driven by the test ----
// Sets the calendar (24 hour, BCD in the registers) and SSR.
void fakeRtcSet(RTC_HandleTypeDef* h, int year, int month, int day, int hour, int minute, int second, uint32_t ssr);
// The timestamp unit latches now (as an edge on RTC_TS would).
void fakeRtcLatch(RTC_HandleTypeDef* h);

#ifdef __cplusplus
}
#endif
