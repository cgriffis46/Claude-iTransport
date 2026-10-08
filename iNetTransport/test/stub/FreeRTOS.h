// Host simulation of the FreeRTOS calls the xNet classes make: queues,
// stream buffers, event groups, ticks and critical sections. Real
// threads (std::thread), with one mutex and condition variable behind
// every object, so a test can have a user thread genuinely asleep in
// client.read() while the driver thread runs. A tick is 1 ms of real
// time. task.h, queue.h, stream_buffer.h, message_buffer.h, event_groups.h and
// semphr.h all just
// include this file.
#pragma once
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
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
inline BaseType_t xQueueReset(QueueHandle_t h) { std::lock_guard<std::mutex> l(simrtos::mu()); h->q.clear(); return pdPASS; }
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

// ---- tasks ----
// xTaskCreate() starts a detached std::thread. A task ends when its
// function returns: vTaskDelete(nullptr) (which a real task must call
// instead of returning) does nothing here. simrtos::failTaskCreate makes
// the next creations fail, as when the FreeRTOS heap is out.
typedef void* TaskHandle_t;
typedef void (*TaskFunction_t)(void*);
typedef uint32_t StackType_t;
#define configSTACK_DEPTH_TYPE uint16_t
#define tskIDLE_PRIORITY ((UBaseType_t)0U)
inline TaskHandle_t xTaskGetCurrentTaskHandle() { static thread_local char me; return &me; }
namespace simrtos {
inline std::atomic<int>& failTaskCreate() { static std::atomic<int> n{0}; return n; }
inline std::atomic<int>& tasksCreated() { static std::atomic<int> n{0}; return n; }
inline std::atomic<int>& tasksRunning() { static std::atomic<int> n{0}; return n; }
}
inline BaseType_t xTaskCreate(TaskFunction_t fn, const char*, configSTACK_DEPTH_TYPE, void* arg, UBaseType_t,
                              TaskHandle_t* handle) {
    if (simrtos::failTaskCreate() > 0) { --simrtos::failTaskCreate(); return pdFAIL; }
    ++simrtos::tasksCreated();
    ++simrtos::tasksRunning();
    std::thread([fn, arg] { fn(arg); --simrtos::tasksRunning(); }).detach();
    if (handle) *handle = nullptr;
    return pdPASS;
}
inline void vTaskDelete(TaskHandle_t) {}

// ---- heap ----
inline void* pvPortMalloc(size_t n) { return std::malloc(n); }
inline void vPortFree(void* p) { std::free(p); }

// ---- semaphores and mutexes (semphr.h) ----
// One kind of object for all three: a mutex is a binary semaphore that
// starts given; a recursive mutex also counts its owner's takes.
struct SimSemaphore { int count; TaskHandle_t owner; int depth; int max; };
typedef SimSemaphore* SemaphoreHandle_t;

inline SemaphoreHandle_t xSemaphoreCreateMutex() { return new SimSemaphore{1, nullptr, 0, 1}; }
inline SemaphoreHandle_t xSemaphoreCreateRecursiveMutex() { return new SimSemaphore{1, nullptr, 0, 1}; }
inline SemaphoreHandle_t xSemaphoreCreateBinary() { return new SimSemaphore{0, nullptr, 0, 1}; }   // starts taken
inline SemaphoreHandle_t xSemaphoreCreateCounting(UBaseType_t max, UBaseType_t initial) {
    return new SimSemaphore{static_cast<int>(initial), nullptr, 0, static_cast<int>(max)};
}
inline UBaseType_t uxSemaphoreGetCount(SemaphoreHandle_t h) {
    std::lock_guard<std::mutex> l(simrtos::mu());
    return static_cast<UBaseType_t>(h->count);
}
inline void vSemaphoreDelete(SemaphoreHandle_t h) { delete h; }
inline BaseType_t xSemaphoreTake(SemaphoreHandle_t h, TickType_t ticks) {
    std::unique_lock<std::mutex> l(simrtos::mu());
    if (!simrtos::waitFor(l, ticks, [&] { return h->count > 0; })) return pdFAIL;
    --h->count;
    return pdPASS;
}
inline BaseType_t xSemaphoreGive(SemaphoreHandle_t h) {
    std::lock_guard<std::mutex> l(simrtos::mu());
    if (h->count >= h->max) return pdFAIL;   // already given
    ++h->count;
    simrtos::cv().notify_all();
    return pdPASS;
}
inline BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t h, TickType_t ticks) {
    const TaskHandle_t me = xTaskGetCurrentTaskHandle();
    std::unique_lock<std::mutex> l(simrtos::mu());
    if (h->owner == me) { ++h->depth; return pdPASS; }
    if (!simrtos::waitFor(l, ticks, [&] { return h->count > 0; })) return pdFAIL;
    h->count = 0;
    h->owner = me;
    h->depth = 1;
    return pdPASS;
}
inline BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t h) {
    std::lock_guard<std::mutex> l(simrtos::mu());
    if (h->owner != xTaskGetCurrentTaskHandle()) return pdFAIL;
    if (--h->depth == 0) {
        h->owner = nullptr;
        h->count = 1;
        simrtos::cv().notify_all();
    }
    return pdPASS;
}

// ---- message buffers (message_buffer.h) ----
// Whole messages, each taking its length plus 4 bytes of the capacity,
// as in FreeRTOS. A receive buffer too small for the next message gets
// nothing, and the message stays.
struct SimMessages { size_t cap, used; std::deque<std::vector<uint8_t>> q; };
typedef SimMessages* MessageBufferHandle_t;

inline MessageBufferHandle_t xMessageBufferCreate(size_t bytes) { return new SimMessages{bytes, 0, {}}; }
inline void vMessageBufferDelete(MessageBufferHandle_t h) { delete h; }
inline size_t xMessageBufferSend(MessageBufferHandle_t h, const void* data, size_t len, TickType_t ticks) {
    std::unique_lock<std::mutex> l(simrtos::mu());
    if (len + 4 > h->cap) return 0;
    if (!simrtos::waitFor(l, ticks, [&] { return h->cap - h->used >= len + 4; })) return 0;
    const uint8_t* p = static_cast<const uint8_t*>(data);
    h->q.emplace_back(p, p + len);
    h->used += len + 4;
    simrtos::cv().notify_all();
    return len;
}
inline size_t xMessageBufferReceive(MessageBufferHandle_t h, void* buf, size_t cap, TickType_t ticks) {
    std::unique_lock<std::mutex> l(simrtos::mu());
    if (!simrtos::waitFor(l, ticks, [&] { return !h->q.empty(); })) return 0;
    const size_t n = h->q.front().size();
    if (n > cap) return 0;
    std::memcpy(buf, h->q.front().data(), n);
    h->q.pop_front();
    h->used -= n + 4;
    simrtos::cv().notify_all();
    return n;
}
inline size_t xMessageBufferSpacesAvailable(MessageBufferHandle_t h) {
    std::lock_guard<std::mutex> l(simrtos::mu());
    return h->cap - h->used;
}
