#pragma once
#include "BMP280Sensor.h"
#include "xSensorBase.h"
#include "cmsis_os2.h"

// FreeRTOS-enabled BMP280. Create THIS instead of plain BMP280Sensor
// whenever a real CMSIS-RTOS2-compatible RTOS is available — the
// state machine itself (main(), inherited unchanged from BMP280Sensor)
// is byte-for-byte identical either way; only sleep()/startThread()/
// StopThread() differ, so this class overrides exactly those three
// and nothing else.
//
// Multiply inherits BMP280Sensor and xSensorBase — safe, not a
// diamond, because xSensorBase shares no ancestor with SensorBase
// (which BMP280Sensor derives from). The three overrides below each
// satisfy BOTH SensorBase's own virtuals (so BMP280Sensor::main()'s
// internal sleep(1)/startThread() calls dispatch here correctly) AND
// xSensorBase's interface (so code that only knows "this is some
// RTOS-capable sensor," via xSensorBase*, can also reach them) — one
// implementation, two interfaces satisfied, same mechanism already
// used for Barometer/Altimeter/Thermometer.
//
// Practical effect of the override: every "still waiting" sleep(1)
// call scattered through BMP280Sensor::main() goes from a true no-op
// to a real osDelay(1) — genuinely yielding the CPU instead of
// depending on the caller to invoke main() again later. And
// startThread() means begin() actually spawns this sensor's own
// dedicated task (threadTrampoline, below) rather than requiring the
// user to drive main() from their own loop().
class xBMP280 : public BMP280Sensor, public xSensorBase {
public:
    explicit xBMP280(ISensorTransport& transport) : BMP280Sensor(transport) {}

    // Explicitly calls StopThread() here, on THIS class's own
    // destructor body — not left to SensorBase's destructor, which
    // (per StopThread()'s own comment) can't reliably reach this
    // override anyway, since by the time ~SensorBase() runs, this
    // object's derived part is already gone. Calling our own override
    // from our own destructor is safe for the same reason calling our
    // own override from our own constructor was: the vtable still
    // correctly points at xBMP280 at this point.
    //
    // Safe to call from any thread now, not just from within
    // threadTrampoline() itself: StopThread() below targets threadId_
    // directly rather than relying on osThreadTerminate(NULL)'s
    // "terminate the calling thread" convention, so it correctly
    // stops THIS sensor's own dedicated task regardless of which
    // thread is running this destructor.
    ~xBMP280() override { StopThread(); }

    // Yields the calling task via osDelay() rather than spinning.
    // Assumes this sensor runs in its own RTOS task (started by
    // startThread() below) rather than sharing a single-threaded
    // superloop with unrelated work. Overrides both SensorBase's and
    // xSensorBase's sleep() — one implementation, both interfaces.
    void sleep(uint32_t ms) override;

    // Spawns this sensor's own dedicated RTOS task via osThreadNew(),
    // whose entry point (threadTrampoline) loops calling main() forever.
    // Called by SensorBase::begin() — see there for when to call it.
    // Stores the resulting handle in threadId_, so StopThread() can
    // target this specific task later. Overrides both SensorBase's
    // and xSensorBase's startThread().
    bool startThread() override;

    // Terminates threadId_ specifically (not osThreadTerminate(NULL),
    // which would terminate whatever thread happens to be calling
    // this) — correct regardless of which thread calls StopThread().
    // Overrides both SensorBase's and xSensorBase's StopThread().
    void StopThread() override;

private:
    static void threadTrampoline(void* arg); // osThreadNew() entry point
    osThreadId_t threadId_ = nullptr;         // set by startThread(), used by StopThread()
};
