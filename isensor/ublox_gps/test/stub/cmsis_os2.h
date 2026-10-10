// A small simulation of the CMSIS-RTOS2 calls FreeRtosTransport uses,
// so it can be tested on a host. One "current thread" at a time; the
// test switches threads and fires interrupts by hand.
#pragma once
#include <cstdint>
#include <functional>
#include <map>

typedef void* osMutexId_t;
typedef void* osThreadId_t;
typedef enum { osOK = 0, osErrorTimeout = -2, osErrorParameter = -4 } osStatus_t;
#define osFlagsWaitAny      0x00000000U
#define osFlagsError        0x80000000U
#define osFlagsErrorTimeout 0xFFFFFFFEU

struct SimRtos {
    osThreadId_t current = nullptr;
    std::map<osThreadId_t, uint32_t> flags;      // per-thread flags, as in the real thing
    std::map<osMutexId_t, osThreadId_t> owner;
    int      blocks = 0;                         // times a thread actually had to block
    int      sets = 0;                           // times an interrupt set a thread's flags
    uint32_t lastTimeout = 0;
    std::function<void()> whileBlocked;          // runs "during" a block: an interrupt arriving
};
inline SimRtos g_rtos;

inline osThreadId_t osThreadGetId(void) { return g_rtos.current; }

inline osStatus_t osMutexAcquire(osMutexId_t m, uint32_t) {
    if (m == nullptr) return osErrorParameter;
    if (g_rtos.owner[m] != nullptr) return osErrorTimeout; // held (not recursive)
    g_rtos.owner[m] = g_rtos.current;
    return osOK;
}
inline osStatus_t osMutexRelease(osMutexId_t m) { g_rtos.owner[m] = nullptr; return osOK; }

inline uint32_t osThreadFlagsSet(osThreadId_t t, uint32_t f) { ++g_rtos.sets; return g_rtos.flags[t] |= f; }

inline uint32_t osThreadFlagsWait(uint32_t f, uint32_t, uint32_t timeout) {
    uint32_t& mine = g_rtos.flags[g_rtos.current];
    if ((mine & f) == 0) {
        ++g_rtos.blocks;
        g_rtos.lastTimeout = timeout;
        if (g_rtos.whileBlocked) { auto irq = g_rtos.whileBlocked; g_rtos.whileBlocked = nullptr; irq(); }
    }
    const uint32_t got = mine & f;
    if (got == 0) return osFlagsErrorTimeout;
    mine &= ~f;
    return got;
}
