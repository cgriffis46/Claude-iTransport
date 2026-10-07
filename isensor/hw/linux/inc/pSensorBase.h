#pragma once
#include <cstdint>

// Pure interface for POSIX-specific sensor behavior (sleep/thread
// management) — deliberately NOT related to SensorBase in any way,
// same reasoning as xSensorBase (its FreeRTOS-flavored sibling):
// SensorBase doesn't know, and shouldn't need to know, whether an OS
// is running underneath it, let alone which one. A sensor that needs
// real POSIX thread behavior (Linux, or any other Unix-like target)
// implements THIS interface alongside its own SensorBase-derived
// hierarchy, rather than SensorBase itself growing an OS-shaped hole.
//
// Same mechanism as xSensorBase: because this shares no ancestor with
// SensorBase, a concrete class (a future pBMP280, say) can multiply
// inherit both — public SensorBase-derived-class, public pSensorBase
// — without creating a diamond. One set of overrides (sleep(),
// startThread(), StopThread()) satisfies both interfaces at once,
// same name and signature, unrelated bases. Proven for xSensorBase's
// identical shape earlier in this project; the same proof applies
// here unchanged.
//
// Expected concrete backing for a POSIX implementation:
//   sleep(ms)     -> usleep(ms * 1000) or nanosleep()
//   startThread() -> pthread_create()
//   StopThread()  -> pthread_cancel() + pthread_join(), or a cleaner
//                    cooperative stop flag the thread checks itself
//                    (pthread_cancel() can leave resources in an
//                    inconsistent state if the thread is mid-syscall
//                    when cancelled — worth deciding deliberately
//                    when a concrete class actually implements this,
//                    not something this interface can dictate)
class pSensorBase {
public:
    virtual ~pSensorBase() = default;

    virtual void sleep(uint32_t ms) = 0;
    virtual bool startThread() = 0;
    virtual void StopThread() = 0;
};
