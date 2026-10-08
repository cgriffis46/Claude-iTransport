// Host simulation of the FreeRTOS calls the xNet classes make: queues,
// stream buffers, event groups, ticks and critical sections. Real
// threads (std::thread), with one mutex and condition variable behind
// every object, so a test can have a user thread genuinely asleep in
// client.read() while the driver thread runs. A tick is 1 ms of real
// time. task.h, queue.h, stream_buffer.h, event_groups.h and semphr.h all just
// include this file.
#pragma once
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

typedef uint32_t      TickType_t;
typedef long          BaseType_t;
typedef unsigned long UBaseType_t;
typedef TickType_t    EventBits_t;

#define pdTRUE  ((BaseType_t)1)
#define pdFALSE ((BaseType_t)0)
#define pdPASS  pdTRUE
#define pdFAIL  pdFALSE
#define portMAX_DELAY      ((TickType_t)0xFFFFFFFFu)
#define portTICK_PERIOD_MS ((TickType_t)1)
#define pdMS_TO_TICKS(ms)  ((TickType_t)(ms))
#define configASSERT(x)    assert(x)
#define portYIELD_FROM_ISR(x) ((void)(x))
#define taskENTER_CRITICAL() simrtos::critical().lock()
#define taskEXIT_CRITICAL()  simrtos::critical().unlock()

namespace simrtos {
inline std::mutex& mu() { static std::mutex m; return m; }
inline std::condition_variable& cv() { static std::condition_variable c; return c; }
inline std::recursive_mutex& critical() { static std::recursive_mutex m; return m; }
inline std::chrono::steady_clock::time_point epoch() {
    static const auto t = std::chrono::steady_clock::now();
    return t;
}
template <typename Pred>
inline bool waitFor(std::unique_lock<std::mutex>& l, TickType_t ticks, Pred p) {
    if (ticks == portMAX_DELAY) { cv().wait(l, p); return true; }
    return cv().wait_for(l, std::chrono::milliseconds(ticks), p);
}
} // namespace simrtos

inline TickType_t xTaskGetTickCount() {
    return static_cast<TickType_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - simrtos::epoch()).count());
}
inline void vTaskDelay(TickType_t t) { std::this_thread::sleep_for(std::chrono::milliseconds(t)); }

// ---- queues ----
struct SimQueue { size_t item, depth; std::deque<std::vector<uint8_t>> q; };
typedef SimQueue* QueueHandle_t;

inline QueueHandle_t xQueueCreate(UBaseType_t depth, UBaseType_t item) { return new SimQueue{item, depth, {}}; }
inline void vQueueDelete(QueueHandle_t h) { delete h; }
inline BaseType_t xQueueSend(QueueHandle_t h, const void* item, TickType_t ticks) {
    std::unique_lock<std::mutex> l(simrtos::mu());
    if (!simrtos::waitFor(l, ticks, [&] { return h->q.size() < h->depth; })) return pdFAIL;
    const uint8_t* p = static_cast<const uint8_t*>(item);
    h->q.emplace_back(p, p + h->item);
    simrtos::cv().notify_all();
    return pdPASS;
}
inline BaseType_t xQueueSendFromISR(QueueHandle_t h, const void* item, BaseType_t* woken) {
    if (woken) *woken = pdTRUE;
    return xQueueSend(h, item, 0);
}
inline BaseType_t xQueueReceive(QueueHandle_t h, void* out, TickType_t ticks) {
    std::unique_lock<std::mutex> l(simrtos::mu());
    if (!simrtos::waitFor(l, ticks, [&] { return !h->q.empty(); })) return pdFAIL;
    std::memcpy(out, h->q.front().data(), h->item);
    h->q.pop_front();
    simrtos::cv().notify_all();
    return pdPASS;
}

// ---- stream buffers ----
struct SimStream { size_t cap, trigger; std::deque<uint8_t> d; };
typedef SimStream* StreamBufferHandle_t;

inline StreamBufferHandle_t xStreamBufferCreate(size_t size, size_t trigger) { return new SimStream{size, trigger ? trigger : 1, {}}; }
inline void vStreamBufferDelete(StreamBufferHandle_t h) { delete h; }
inline size_t xStreamBufferSend(StreamBufferHandle_t h, const void* data, size_t len, TickType_t ticks) {
    std::unique_lock<std::mutex> l(simrtos::mu());
    simrtos::waitFor(l, ticks, [&] { return h->d.size() < h->cap; });
    const size_t n = len < h->cap - h->d.size() ? len : h->cap - h->d.size();
    const uint8_t* p = static_cast<const uint8_t*>(data);
    h->d.insert(h->d.end(), p, p + n);
    if (n) simrtos::cv().notify_all();
    return n;
}
inline size_t xStreamBufferReceive(StreamBufferHandle_t h, void* buf, size_t len, TickType_t ticks) {
    std::unique_lock<std::mutex> l(simrtos::mu());
    simrtos::waitFor(l, ticks, [&] { return h->d.size() >= h->trigger; });
    size_t n = len < h->d.size() ? len : h->d.size();
    uint8_t* p = static_cast<uint8_t*>(buf);
    for (size_t i = 0; i < n; ++i) { p[i] = h->d.front(); h->d.pop_front(); }
    if (n) simrtos::cv().notify_all();
    return n;
}
inline size_t xStreamBufferBytesAvailable(StreamBufferHandle_t h) { std::lock_guard<std::mutex> l(simrtos::mu()); return h->d.size(); }
inline size_t xStreamBufferSpacesAvailable(StreamBufferHandle_t h) { std::lock_guard<std::mutex> l(simrtos::mu()); return h->cap - h->d.size(); }
inline BaseType_t xStreamBufferIsEmpty(StreamBufferHandle_t h) { std::lock_guard<std::mutex> l(simrtos::mu()); return h->d.empty() ? pdTRUE : pdFALSE; }
inline BaseType_t xStreamBufferReset(StreamBufferHandle_t h) { std::lock_guard<std::mutex> l(simrtos::mu()); h->d.clear(); return pdPASS; }

// ---- event groups ----
struct SimEvents { EventBits_t bits; };
typedef SimEvents* EventGroupHandle_t;

inline EventGroupHandle_t xEventGroupCreate() { return new SimEvents{0}; }
inline void vEventGroupDelete(EventGroupHandle_t h) { delete h; }
inline EventBits_t xEventGroupSetBits(EventGroupHandle_t h, EventBits_t b) {
    std::lock_guard<std::mutex> l(simrtos::mu());
    h->bits |= b;
    simrtos::cv().notify_all();
    return h->bits;
}
inline EventBits_t xEventGroupClearBits(EventGroupHandle_t h, EventBits_t b) {
    std::lock_guard<std::mutex> l(simrtos::mu());
    const EventBits_t before = h->bits;
    h->bits &= ~b;
    return before;
}
inline EventBits_t xEventGroupGetBits(EventGroupHandle_t h) { std::lock_guard<std::mutex> l(simrtos::mu()); return h->bits; }
inline EventBits_t xEventGroupWaitBits(EventGroupHandle_t h, EventBits_t want, BaseType_t clear,
                                       BaseType_t all, TickType_t ticks) {
    std::unique_lock<std::mutex> l(simrtos::mu());
    auto met = [&] { return all ? (h->bits & want) == want : (h->bits & want) != 0; };
    const bool ok = simrtos::waitFor(l, ticks, met);
    const EventBits_t got = h->bits;
    if (ok && clear) h->bits &= ~want;
    return got;
}

// ---- mutexes (semphr.h) ----
struct SimMutex { bool held; };
typedef SimMutex* SemaphoreHandle_t;

inline SemaphoreHandle_t xSemaphoreCreateMutex() { return new SimMutex{false}; }
inline void vSemaphoreDelete(SemaphoreHandle_t h) { delete h; }
inline BaseType_t xSemaphoreTake(SemaphoreHandle_t h, TickType_t ticks) {
    std::unique_lock<std::mutex> l(simrtos::mu());
    if (!simrtos::waitFor(l, ticks, [&] { return !h->held; })) return pdFAIL;
    h->held = true;
    return pdPASS;
}
inline BaseType_t xSemaphoreGive(SemaphoreHandle_t h) {
    std::lock_guard<std::mutex> l(simrtos::mu());
    h->held = false;
    simrtos::cv().notify_all();
    return pdPASS;
}
