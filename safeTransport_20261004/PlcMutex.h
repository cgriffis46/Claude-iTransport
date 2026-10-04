#pragma once

// A minimal, portable mutex abstraction — NOT a stand-in for
// std::mutex out of caution, but because std::mutex is CONFIRMED not
// to work on this project's actual STM32 target: verified directly,
// not assumed, by cross-compiling PlcTagRegistry against the real
// STM32F207ZG_SafeRelay project with arm-none-eabi-g++. The toolchain
// reports "Thread model: single", and <mutex> fails outright
// ('mutex' is not a member of 'std'). This is exactly the risk
// PlcTagRegistry's own header once flagged as "not yet verified" —
// now confirmed as a real, blocking problem with std::mutex
// specifically on this toolchain, not a hypothetical caveat.
//
// Wraps the right underlying primitive per platform at compile time:
// CMSIS-RTOS2's osMutexId_t under FreeRTOS (when
// PLC_MUTEX_USE_CMSIS_RTOS2 is defined for the build), std::mutex
// everywhere else (Linux/KR260, where the standard C++ threading
// library is genuinely available and tested — see PlcTagRegistry's
// own multi-threaded test, which exercises exactly that path).
//
// Deliberately exposes only lock()/unlock() — the same minimal
// surface std::lock_guard<T> actually requires — so
// std::lock_guard<PlcMutex> works completely unchanged wherever
// std::lock_guard<std::mutex> was used before; PlcTagRegistry's own
// locking code didn't need to change shape, only which type it names.
#if defined(PLC_MUTEX_USE_CMSIS_RTOS2)
#include "cmsis_os2.h"

class PlcMutex {
public:
    PlcMutex() : handle_(osMutexNew(nullptr)) {}
    ~PlcMutex() { if (handle_) osMutexDelete(handle_); }
    PlcMutex(const PlcMutex&) = delete;
    PlcMutex& operator=(const PlcMutex&) = delete;

    void lock()   { osMutexAcquire(handle_, osWaitForever); }
    void unlock() { osMutexRelease(handle_); }

private:
    osMutexId_t handle_;
};

#else
#include <mutex>

class PlcMutex {
public:
    void lock()   { mutex_.lock(); }
    void unlock() { mutex_.unlock(); }

private:
    std::mutex mutex_;
};
#endif
