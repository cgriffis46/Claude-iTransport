#pragma once
#include "BMP280Sensor.h"
#include "pSensorBase.h"
#include <atomic>
#include <pthread.h>

// POSIX-backed BMP280. Create THIS instead of plain BMP280Sensor on a
// Linux/Unix target with real pthreads available — the state machine
// itself (main(), inherited unchanged from BMP280Sensor) is identical
// either way; only sleep()/startThread()/StopThread() differ.
//
// Multiply inherits BMP280Sensor and pSensorBase — safe, not a
// diamond, same reasoning as xBMP280/xSensorBase: pSensorBase shares
// no ancestor with SensorBase. The three overrides below each satisfy
// BOTH SensorBase's own virtuals (so BMP280Sensor::main()'s internal
// sleep(1)/startThread() calls dispatch here correctly) AND
// pSensorBase's interface — one implementation, two interfaces.
//
// StopThread() uses a cooperative flag, NOT pthread_cancel(): this
// class's main loop can be mid-ioctl() (via KR260I2cTransport, say)
// when a stop is requested, and pthread_cancel() can leave a syscall
// — or whatever resource it touched — in an inconsistent state if
// cancelled mid-flight. A cooperative flag is always safe, at the
// cost of StopThread() only returning once the thread notices the
// flag and finishes whatever it's currently doing — not instant the
// way pthread_cancel() would be.
class pBMP280 : public BMP280Sensor, public pSensorBase {
public:
    explicit pBMP280(ISensorTransport& transport) : BMP280Sensor(transport) {}

    // Requests a stop and waits (pthread_join()) for the thread to
    // actually exit before this object finishes destructing — same
    // reasoning as xBMP280's destructor: SensorBase's own destructor
    // can't reliably reach this override (see StopThread()'s comment
    // on SensorBase for why), so this class needs its own.
    ~pBMP280() override { StopThread(); }

    // nanosleep()-based — the POSIX-preferred replacement for the
    // obsolete usleep(). Loops on EINTR so a signal doesn't cut the
    // wait short. Overrides both SensorBase's and pSensorBase's
    // sleep() — one implementation, both interfaces.
    void sleep(uint32_t ms) override;

    // pthread_create()'s entry point (threadTrampoline) loops calling
    // main() forever until stopRequested_ is set. Overrides both
    // SensorBase's and pSensorBase's startThread().
    bool startThread() override;

    // Sets stopRequested_, then pthread_join()s until the thread
    // actually exits. See the class comment above for why this is
    // cooperative rather than pthread_cancel()-based. Overrides both
    // SensorBase's and pSensorBase's StopThread(). Safe to call more
    // than once (a no-op if the thread was never started, or already stopped).
    void StopThread() override;

    // Installs handlers for SIGTERM and SIGINT that set a shared,
    // process-wide flag every pBMP280 instance's thread loop checks
    // (in addition to its own per-instance stopRequested_) — so
    // receiving either signal makes every running pBMP280's thread
    // notice and exit gracefully, the way calling StopThread() would,
    // rather than the process just dying mid-operation with no chance
    // to run destructors at all (the default disposition for both
    // signals is immediate termination — no stack unwinding, no
    // destructors, nothing).
    //
    // NOT called automatically by the constructor: this installs a
    // PROCESS-WIDE handler via sigaction(), which would silently
    // override — or be overridden by — any signal handling the rest
    // of the application does for the same signals. Call this once,
    // explicitly, from application startup code if you want it, e.g.
    // right after main() begins, before any pBMP280 is constructed.
    static void installDefaultSignalHandlers();

private:
    static void* threadTrampoline(void* arg); // pthread_create() entry point
    static uint32_t monotonicMillis();          // main()'s nowMs, via CLOCK_MONOTONIC
    static void handleTerminationSignal(int signum); // sigaction() handler — signal-safe only

    pthread_t         threadId_ = 0;
    std::atomic<bool> stopRequested_{false};
    bool              threadStarted_ = false; // guards against joining a never-started thread

    // Process-wide, shared by every pBMP280 instance — see
    // installDefaultSignalHandlers()'s comment for why this can't be
    // per-instance the way stopRequested_ is.
    static std::atomic<bool> s_signalReceived;
};
