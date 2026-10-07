#pragma once
#include <cstdint>

// Pure interface for RTOS-specific sensor behavior (sleep/thread
// management) — deliberately NOT related to SensorBase in any way.
// SensorBase doesn't know, and shouldn't need to know, whether an OS
// is running underneath it at all, let alone which one — a sensor
// that needs real RTOS behavior implements this interface ALONGSIDE
// its own SensorBase-derived hierarchy, rather than SensorBase itself
// growing an OS-shaped hole.
//
// Because this shares no ancestor with SensorBase, a concrete class
// (xBMP280, or a future xAnythingElse) can multiply inherit both
// without creating a diamond. A single set of overrides in that
// class — one sleep(), one startThread(), one StopThread() — collapses
// into overriding both this interface's virtuals AND SensorBase's own:
// same name, same signature, unrelated bases, one implementation
// satisfies both. Proven with a standalone test before writing this;
// see the conversation this came from if that ever needs re-checking.
class xSensorBase {
public:
    virtual ~xSensorBase() = default;

    virtual void sleep(uint32_t ms) = 0;
    virtual bool startThread() = 0;
    virtual void StopThread() = 0;
};
