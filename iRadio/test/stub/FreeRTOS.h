// Single threaded stand-in for the FreeRTOS calls xdavis_rfm69.h makes,
// for the host test. Nothing runs on its own: the test calls step(), and
// time moves only while the radio task sleeps in ulTaskNotifyTake(), one
// ms at a time, calling g_onTick for each ms so the simulated world
// (stations, radio chip, consumers) moves with it. A notification given
// during that (from the DIO0 "interrupt" or a command) ends the sleep, as
// it would on the target. task.h, queue.h and stream_buffer.h include this.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <vector>

typedef uint32_t TickType_t;
typedef long BaseType_t;
typedef unsigned long UBaseType_t;
typedef void (*TaskFunction_t)(void*);

#define pdTRUE  ((BaseType_t)1)
#define pdFALSE ((BaseType_t)0)
#define pdPASS  pdTRUE
#define pdFAIL  pdFALSE
#define portMAX_DELAY ((TickType_t)0xFFFFFFFFu)
#define portTICK_PERIOD_MS ((TickType_t)1)
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define tskIDLE_PRIORITY ((UBaseType_t)0)
#define portYIELD_FROM_ISR(x) ((void)(x))

namespace stubrtos {
struct Task { TaskFunction_t fn; const char* name; uint16_t stack; void* arg; UBaseType_t prio; uint32_t notify; };
inline TickType_t& tick() { static TickType_t t = 0; return t; }
inline std::function<void(TickType_t)>& onTick() { static std::function<void(TickType_t)> f; return f; }
inline Task& task() { static Task t = {nullptr, nullptr, 0, nullptr, 0, 0}; return t; }
inline int& created() { static int n = 0; return n; }
inline uint32_t& longestSleep() { static uint32_t n = 0; return n; }
} // namespace stubrtos

typedef stubrtos::Task* TaskHandle_t;

inline TickType_t xTaskGetTickCount() { return stubrtos::tick(); }
inline TickType_t xTaskGetTickCountFromISR() { return stubrtos::tick(); }
inline TaskHandle_t xTaskGetCurrentTaskHandle() { return &stubrtos::task(); }

inline BaseType_t xTaskCreate(TaskFunction_t fn, const char* name, uint16_t stack, void* arg, UBaseType_t prio, TaskHandle_t* out) {
    stubrtos::Task& t = stubrtos::task();
    t.fn = fn; t.name = name; t.stack = stack; t.arg = arg; t.prio = prio;
    ++stubrtos::created();
    if (out) *out = &t;
    return pdPASS;
}

inline void xTaskNotifyGive(TaskHandle_t t) { ++t->notify; }
inline void vTaskNotifyGiveFromISR(TaskHandle_t t, BaseType_t* woken) { ++t->notify; if (woken) *woken = pdTRUE; }

inline uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t ticks) {
    stubrtos::Task& t = stubrtos::task();
    uint32_t slept = 0;
    while (t.notify == 0 && slept < ticks) {
        ++stubrtos::tick();
        ++slept;
        if (stubrtos::onTick()) stubrtos::onTick()(stubrtos::tick());
    }
    if (slept > stubrtos::longestSleep()) stubrtos::longestSleep() = slept;
    const uint32_t n = t.notify;
    if (n) t.notify = clear ? 0 : n - 1;
    return n;
}

// ---- queues ----
struct StubQueue { size_t item, depth; std::deque<std::vector<uint8_t>> q; };
typedef StubQueue* QueueHandle_t;
inline QueueHandle_t xQueueCreate(UBaseType_t depth, UBaseType_t item) { return new StubQueue{item, depth, {}}; }
inline BaseType_t xQueueSend(QueueHandle_t h, const void* item, TickType_t) {
    if (h->q.size() >= h->depth) return pdFAIL;     // single threaded: nobody can make room while it waits
    const uint8_t* p = static_cast<const uint8_t*>(item);
    h->q.emplace_back(p, p + h->item);
    return pdPASS;
}
inline BaseType_t xQueueReceive(QueueHandle_t h, void* out, TickType_t) {
    if (h->q.empty()) return pdFAIL;
    std::memcpy(out, h->q.front().data(), h->item);
    h->q.pop_front();
    return pdPASS;
}
inline UBaseType_t uxQueueMessagesWaiting(QueueHandle_t h) { return (UBaseType_t)h->q.size(); }

// ---- stream buffers ----
struct StubStream { size_t size; std::deque<uint8_t> b; };
typedef StubStream* StreamBufferHandle_t;
inline StreamBufferHandle_t xStreamBufferCreate(size_t size, size_t) { return new StubStream{size, {}}; }
inline size_t xStreamBufferSpacesAvailable(StreamBufferHandle_t s) { return s->size - s->b.size(); }
inline size_t xStreamBufferBytesAvailable(StreamBufferHandle_t s) { return s->b.size(); }
inline size_t xStreamBufferSend(StreamBufferHandle_t s, const void* data, size_t n, TickType_t) {
    const size_t k = n < xStreamBufferSpacesAvailable(s) ? n : xStreamBufferSpacesAvailable(s);
    const uint8_t* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < k; ++i) s->b.push_back(p[i]);
    return k;
}
inline size_t xStreamBufferReceive(StreamBufferHandle_t s, void* out, size_t n, TickType_t) {
    size_t k = 0;
    uint8_t* p = static_cast<uint8_t*>(out);
    while (k < n && !s->b.empty()) { p[k++] = s->b.front(); s->b.pop_front(); }
    return k;
}
