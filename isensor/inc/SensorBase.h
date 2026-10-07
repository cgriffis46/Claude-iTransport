#pragma once
#include <cstdint>
#include "ISensorTransport.h"

// SensorBase owns the non-blocking state machine (Template Method
// pattern). The sequence of states — and the fact that none of them
// block — is fixed here and never changes between sensors.
//
// A concrete sensor (BME280Sensor, MPU6050Sensor, ...) only fills in
// the five hooks below: which registers to touch, how long conversion
// takes, and how to decode raw bytes into real units. It never touches
// I2C/SPI directly — that's the injected ISensorTransport's job.
//
// Wiring a sensor to a bus is just constructor injection:
//   I2CTransport bus(0x76);
//   BME280Sensor  bme(bus);
//   ... bme.main(millis()) every loop() pass ...
class SensorBase {
public:
    // Constructor explicitly puts the state machine into its first
    // step (Calibrate) — every SensorBase starts there, deliberately,
    // not just because that happens to be state_'s default value.
    // Safe to do plainly like this (rather than via a virtual call)
    // precisely because Calibrate is just an enum value here, not a
    // hook — hasCalibrationData() etc. are only ever called from
    // main(), never from this constructor.
    explicit SensorBase(ISensorTransport& transport) : transport_(transport), state_(State::Calibrate) {}
    // Calls StopThread() — see that method's own comment for why this
    // alone won't reach a derived class's override; SensorBase's own
    // (no-op) version is what actually runs here in most cases.
    virtual ~SensorBase() { StopThread(); }

    // Call every loop() pass. Never blocks, regardless of what state
    // the underlying transfer is in. One call = one state-machine step.
    // Virtual so a future sensor can override the whole state machine
    // instead of using this generic one — most sensors won't need to.
    virtual void main(uint32_t nowMs);

    bool hasNewReading() const { return newReadingReady_; }
    void clearNewReading()     { newReadingReady_ = false; }

    // Optional callback, invoked once per completed reading (right
    // after Process, before returning to Idle) — an alternative to
    // polling hasNewReading()/clearNewReading() from the caller's own
    // loop. Plain function pointer + opaque context, not std::function,
    // to avoid any heap allocation. Unset by default; if never called,
    // SensorBase::State::NotifyData is a single no-op step.
    using NewDataCallback = void (*)(SensorBase& sensor, void* context);
    void setNewDataCallback(NewDataCallback callback, void* context = nullptr) {
        newDataCallback_ = callback;
        callbackContext_ = context;
    }

    // Call once, after construction, before the RTOS scheduler starts
    // (or before the first main() call, on a platform without one).
    // Starts this sensor's own dedicated thread if it needs one (see
    // startThread() below). Unlike calling a virtual from inside a
    // constructor, this dispatches correctly no matter how deep the
    // inheritance chain is — begin() runs after construction has
    // fully finished, so the vtable is exactly what it should be.
    void begin() { startThread(); }

    // Hardware reset hook — e.g. writing a documented soft-reset word
    // to a reset register (BMP280: 0xB6 -> reg 0xE0). Returns true once
    // the write has been *issued* (non-blocking), same convention as
    // ISensorTransport::writeReg(). Default is a no-op that reports
    // "nothing to do" so sensors without a reset procedure aren't
    // forced to implement one. Override in a subclass that has one.
    virtual bool reset() { return true; }

    // Issues a read of the sensor's factory calibration/trim data, if
    // it has any. Same convention as reset(): returns true once the
    // read has been *issued*, not once it's landed — the caller polls
    // isBusy()/lastOpFailed() separately, same as everywhere else.
    // Default is a blank stub that does nothing and reports "nothing
    // to load" — most sensors don't ship calibration data. Sensors
    // that do (most Bosch parts: BMP280, BME280, ...) override this.
    virtual bool loadCalibration() { return true; }

    // Issues a one-time device configuration write — e.g. setting an
    // IIR filter / standby-time config register. Same convention as
    // reset()/loadCalibration(): returns true once *issued*, not once
    // landed. Default is a no-op ("nothing to configure").
    virtual bool configureDevice() { return true; }

    // Called whenever a state has nothing productive to do this pass
    // and is just waiting — for a fixed duration (conversion time,
    // backoff) or for an async transfer to finish (isBusy() still
    // true). Default is a no-op: safe for a bare superloop, where
    // main() must return immediately and gets called again next pass.
    // Override for an RTOS target to call the platform's task-level
    // yield/sleep primitive (STM32 + CMSIS-RTOS: osDelay()) so the
    // calling task yields the CPU instead of spinning it — "ms" is
    // then interpreted as RTOS ticks, not necessarily wall-clock ms.
    virtual void sleep(uint32_t ms) { (void)ms; }

    // Query variant: given when a fixed-duration wait started and how
    // long it should last, returns how many ms remain — 0 once the
    // duration has fully elapsed. Pure query, never blocks on its
    // own; the caller decides what to do with a nonzero result (e.g.
    // osDelay() for exactly that long, on a platform that has one).
    // The default can't know elapsed time without a real clock, so it
    // always reports "done." Override on a platform where sleep(ms)
    // above has one (same override typically backs both).
    virtual uint32_t sleep(uint32_t startTime, uint32_t sleepTimeMs) {
        (void)startTime; (void)sleepTimeMs;
        return 0;
    }

    // Convenience: composes the two overloads above. Queries how much
    // of [startTime, startTime+sleepTimeMs) is left, then blocks for
    // exactly that long via sleep(ms) — never touches a platform
    // primitive (osDelay or otherwise) directly, so a state using this
    // stays correct under whatever sleep(ms) a subclass provides,
    // including the bare-metal no-op default (where this becomes a
    // true no-op end to end, same as always).
    void sleepUntilElapsed(uint32_t startTime, uint32_t sleepTimeMs) {
        const uint32_t remaining = sleep(startTime, sleepTimeMs);
        if (remaining > 0) sleep(remaining);
    }

    // Attempts to start this sensor's own dedicated RTOS thread. Not
    // driven by the state machine — a subclass calls this itself
    // (typically from its own constructor, once it's safe to do so)
    // rather than SensorBase invoking it automatically. Default is a
    // no-op that reports "nothing to start" (true) — safe for a bare
    // superloop, or a sensor whose thread is wired up externally.
    // Override to spawn a real thread (e.g. STM32 + CMSIS-RTOS:
    // osThreadNew()).
    virtual bool startThread() { return true; }

    // Mirror of startThread() above: stops this sensor's own
    // dedicated thread, if it has one. Default is a no-op ("nothing
    // to stop"). Called from THIS class's destructor below — but note
    // that alone is not sufficient for a further-derived override
    // (xBMP280's, say) to actually run: by the time ~SensorBase()'s
    // body executes, if a derived destructor already ran, the vtable
    // no longer points at that derived class — same "virtuals don't
    // reach derived overrides" issue as constructors, just the
    // destruction-side mirror of it. A subclass that overrides this
    // needs its OWN destructor calling StopThread() directly to
    // reliably stop its own thread; see xBMP280's destructor.
    virtual void StopThread() {}

    // Calibrate is the initial state for every sensor. Sensors that
    // don't need it (hasCalibrationData() == false, the default) fall
    // straight through to Configure on the first main() call — never in
    // the constructor, since hasCalibrationData() is virtual and the
    // derived class's vtable isn't safely callable until construction
    // has finished. (startThread() above is the one exception to that
    // rule — see BMP280Sensor's constructor for why it's safe there.)
    enum class State { Calibrate, Configure, Idle, Trigger, WaitConv, Read, Process, NotifyData, Error };
    State state() const { return state_; }

protected:
    // ---- hooks a concrete sensor must implement ----
    virtual uint8_t  triggerRegister() const = 0;
    virtual uint8_t  triggerValue() const = 0;
    virtual uint8_t  dataRegister() const = 0;
    virtual uint8_t  dataLength() const = 0;      // must be <= sizeof(raw_)
    virtual uint32_t conversionTimeMs() const = 0;
    virtual void     decode(const uint8_t* raw) = 0; // fill in sensor-specific fields

    // ---- optional one-time calibration, run before Configure ----
    // hasCalibrationData() decides the branch: true routes through
    // loadCalibration() (issues the read) then onCalibrationRead()
    // (parses it — the subclass owns wherever loadCalibration() wrote
    // the bytes, so no buffer is passed here). false skips straight to
    // Configure. Leave both at their defaults for sensors that don't
    // ship calibration data (ExampleTempSensor).
    virtual bool hasCalibrationData() const { return false; }
    virtual void onCalibrationRead() {}

    // Optional extra confirmation, beyond the fixed conversionTimeMs()
    // timer, that a conversion has genuinely finished — for sensors
    // that expose a real busy/measuring status bit. Mirrors the
    // calibration hooks above: hasMeasuringFlag() is the capability
    // check, checkMeasuring() issues the status read (same
    // issue/poll convention as loadCalibration()), and stillMeasuring()
    // interprets the result once it lands. Default: no such bit
    // exists, so conversionTimeMs() alone remains authoritative —
    // exactly the prior behavior.
    virtual bool hasMeasuringFlag() const { return false; }
    virtual bool checkMeasuring() { return false; }
    virtual bool stillMeasuring() const { return false; }

    // ---- tunables a subclass may override; sane defaults given ----
    virtual uint32_t pollIntervalMs() const { return 900; }
    virtual uint32_t i2cTimeoutMs()   const { return 300; }
    virtual uint32_t backoffMs()      const { return 500; }
    virtual uint8_t  maxRetry()       const { return 3; }

    ISensorTransport& transport_;

private:
    static constexpr uint8_t kMaxDataLen = 8;

    State    state_;   // set explicitly in the constructor — see there
    uint32_t tMark_ = 0;
    uint8_t  raw_[kMaxDataLen] = {0};
    bool     calibRequested_     = false;
    bool     configureRequested_ = false;
    bool     measuringCheckPending_ = false;
    uint8_t  retry_ = 0;
    bool     newReadingReady_ = false;
    NewDataCallback newDataCallback_ = nullptr;
    void*           callbackContext_ = nullptr;
};
