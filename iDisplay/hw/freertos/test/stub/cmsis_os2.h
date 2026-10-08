/*
 * Stand-in for the real cmsis_os2.h so xGui.cpp builds and runs on a
 * host, single threaded. The message queue is real enough to show the
 * flow control: a fixed number of slots, and a put that finds them full
 * fails at once. osThreadNew() records the thread but does not run it;
 * the test calls xGui::begin() and runOnce() itself.
 */
#ifndef CMSIS_OS2_STUB_H_
#define CMSIS_OS2_STUB_H_

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <deque>
#include <vector>

typedef enum { osOK = 0, osError = -1, osErrorTimeout = -2, osErrorResource = -3, osErrorParameter = -4 } osStatus_t;
typedef enum { osPriorityNormal = 24, osPriorityAboveNormal = 32 } osPriority_t;
#define osWaitForever 0xFFFFFFFFu

typedef void (*osThreadFunc_t)(void* argument);
typedef void* osThreadId_t;
typedef struct {
    const char* name;
    uint32_t attr_bits;
    void* cb_mem;
    uint32_t cb_size;
    void* stack_mem;
    uint32_t stack_size;
    osPriority_t priority;
    uint32_t tz_module;
    uint32_t reserved;
} osThreadAttr_t;

struct StubQueue {
    uint32_t depth, size;
    std::deque<std::vector<uint8_t>> items;
};
typedef StubQueue* osMessageQueueId_t;
typedef struct { const char* name; } osMessageQueueAttr_t;

extern uint32_t g_tick;                 // osKernelGetTickCount()
extern uint32_t g_slept;                // total osDelay()
extern std::vector<uint32_t> g_delays;
extern int g_threadsCreated;
extern uint32_t g_lastStackSize;
extern bool g_failQueueNew;

inline uint32_t osKernelGetTickCount() { return g_tick; }

inline osStatus_t osDelay(uint32_t ticks) {
    g_slept += ticks;
    g_tick += ticks;
    g_delays.push_back(ticks);
    return osOK;
}

inline osThreadId_t osThreadNew(osThreadFunc_t, void*, const osThreadAttr_t* attr) {
    ++g_threadsCreated;
    g_lastStackSize = attr ? attr->stack_size : 0;
    return (osThreadId_t)(uintptr_t)g_threadsCreated;
}

inline osMessageQueueId_t osMessageQueueNew(uint32_t count, uint32_t size, const osMessageQueueAttr_t*) {
    if (g_failQueueNew) return nullptr;
    StubQueue* q = new StubQueue;
    q->depth = count;
    q->size = size;
    return q;
}

// Single threaded, so nothing can make room while a put waits: a full
// queue fails whatever the timeout (osErrorTimeout when one was given,
// as the real call would after waiting it out).
inline osStatus_t osMessageQueuePut(osMessageQueueId_t q, const void* msg, uint8_t, uint32_t timeout) {
    if (q == nullptr) return osErrorParameter;
    if (q->items.size() >= q->depth) return timeout == 0 ? osErrorResource : osErrorTimeout;
    const uint8_t* p = static_cast<const uint8_t*>(msg);
    q->items.emplace_back(p, p + q->size);
    return osOK;
}

inline osStatus_t osMessageQueueGet(osMessageQueueId_t q, void* msg, uint8_t* prio, uint32_t timeout) {
    if (q == nullptr) return osErrorParameter;
    if (prio) *prio = 0;
    if (q->items.empty()) return timeout == 0 ? osErrorResource : osErrorTimeout;
    memcpy(msg, q->items.front().data(), q->size);
    q->items.pop_front();
    return osOK;
}

inline uint32_t osMessageQueueGetCount(osMessageQueueId_t q) { return q ? (uint32_t)q->items.size() : 0; }

#endif
