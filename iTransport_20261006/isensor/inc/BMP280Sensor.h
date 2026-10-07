#pragma once
#include "SensorBase.h"
#include "Barometer.h"
#include "Altimeter.h"
#include "Thermometer.h"

// BMP280 (Bosch) pressure + temperature sensor.
//
// Register map and compensation formulas per the Bosch BMP280
// datasheet. Uses forced mode with x1 oversampling on both channels —
// change kCtrlMeasForced if you want higher oversampling (longer
// conversionTimeMs() would then be required too).
//
// Implements Barometer, Altimeter, and Thermometer alongside
// SensorBase — multiple inheritance, but a safe case: none of the
// three interfaces carry any state, and none shares an ancestor with
// SensorBase or each other, so there's no diamond to worry about.
class BMP280Sensor : public SensorBase, public Barometer, public Altimeter, public Thermometer {
public:
    // Constructor explicitly puts BMP280's own power-on state machine
    // into its first step (CheckDevice) — same reasoning as
    // SensorBase's own constructor: safe as a plain enum value here,
    // since checkDevice()/reset()/etc. are only ever called from
    // main(), never from this constructor.
    explicit BMP280Sensor(ISensorTransport& transport) : SensorBase(transport), powerPhase_(PowerPhase::CheckDevice) {}

    float lastTemperatureC() const { return lastTempC_; }
    float lastPressurePa()   const { return lastPressurePa_; }

    // Barometer
    float getPressure() const override;

    // Altimeter — see the .cpp for the formula and its accuracy caveat.
    // Defaults to the standard-atmosphere sea-level reference (101325 Pa);
    // override it with a real local value for anything beyond a rough estimate.
    float getAltitude() const override;
    void  setSeaLevelReferencePa(float pa) { seaLevelPa_ = pa; }

    // Thermometer
    float getTemperature() const override;

    // Same idea, for Thermometer.
    Thermometer*       asThermometer()       { return this; }
    const Thermometer* asThermometer() const { return this; }

    // Upcasts to Barometer& — a plain, always-safe pointer adjustment
    // (BMP280Sensor publicly inherits Barometer, so this never fails
    // or needs dynamic_cast). Useful for handing this object to code
    // that depends on Barometer* without knowing BMP280Sensor exists.
    Barometer*       asBarometer()       { return this; }
    const Barometer* asBarometer() const { return this; }

    // Same idea, for Altimeter.
    Altimeter*       asAltimeter()       { return this; }
    const Altimeter* asAltimeter() const { return this; }

    // True once CheckDevice has confirmed nothing responds on the bus.
    // Latched — the sensor stops issuing any further bus traffic once
    // this is set, since there's nothing to talk to.
    bool isDeviceMissing() const { return powerPhase_ == PowerPhase::DeviceNotFound; }

    // Overridden: BMP280 must confirm im_update == 0 (NVM->image-register
    // copy finished) before it's safe to enter the generic
    // Calibrate/Configure/Idle/... cycle inherited from SensorBase.
    // Delegates down to SensorBase::main() once that's confirmed.
    //
    // No RTOS dependency anywhere in this class: sleep()/startThread()
    // are left at SensorBase's no-op defaults, so this only makes
    // forward progress when something calls main() repeatedly — the
    // user's own loop(), calling bmp.main(millis()) (or equivalent)
    // every pass. For a target with a real RTOS available, use
    // xBMP280 instead — same state machine, but with sleep()/
    // startThread() overridden to actually use it (including running
    // in its own dedicated task rather than needing a manual loop()).
    void main(uint32_t nowMs) override;

    // Writes the datasheet's soft-reset word (0xB6) to the reset
    // register (0xE0). Any other value is ignored by the chip per the
    // datasheet, so this is the only value that ever makes sense here.
    bool reset() override;

    // Issues the burst read of the 24-byte factory trim table starting
    // at 0x88, into this class's own calibRaw_ buffer. Called by
    // SensorBase's Calibrate state; onCalibrationRead() parses the
    // result once it lands.
    bool loadCalibration() override;

protected:
    // ---- one-time factory calibration (SensorBase::State::Calibrate) ----
    bool hasCalibrationData() const override { return true; }
    void onCalibrationRead() override; // parses this->calibRaw_, filled in by loadCalibration()

    // ---- extra conversion-complete confirmation (SensorBase::State::WaitConv) ----
    // measuring (status bit 3) is true while a conversion is actually
    // in progress — a real hardware signal, unlike conversionTimeMs()'s
    // fixed estimate. Reuses statusByte_/kRegStatus, already declared
    // below for WaitImUpdate; the two never run concurrently (one is
    // power-on-only, the other steady-state), so sharing is safe.
    bool hasMeasuringFlag() const override { return true; }
    bool checkMeasuring() override { return transport_.readRegs(kRegStatus, &statusByte_, 1); }
    bool stillMeasuring() const override { return (statusByte_ & kMeasuringBit) != 0; }

    // Writes the config register (0xF5): standby time (t_sb, bits
    // [7:5], irrelevant in forced mode), IIR filter (bits [4:2], off
    // here since we're not filtering), spi3w_en (bit 1, off — 4-wire).
    // Runs in SensorBase::State::Configure, right after Calibrate.
    bool configureDevice() override;

    // ---- per-cycle measurement (SensorBase::State machine) ----
    uint8_t  triggerRegister()  const override { return kRegCtrlMeas; }
    uint8_t  triggerValue()     const override { return kCtrlMeasForced; }
    uint8_t  dataRegister()     const override { return kRegDataStart; }
    uint8_t  dataLength()       const override { return 6; } // press(3) + temp(3)
    uint32_t conversionTimeMs() const override { return 10; } // datasheet max ~6.4ms @x1/x1, +margin
    void     decode(const uint8_t* raw) override;

private:
    static constexpr uint8_t kRegCalibStart = 0x88;
    static constexpr uint8_t kCalibLen      = 24;   // dig_T1..T3, dig_P1..P9 (12 x uint16)
    static constexpr uint8_t kRegCtrlMeas   = 0xF4;
    static constexpr uint8_t kRegDataStart  = 0xF7; // press_msb..temp_xlsb, burst-readable
    static constexpr uint8_t kRegStatus     = 0xF3;
    static constexpr uint8_t kImUpdateBit   = 0x01; // bit 0: 1 while NVM->image-reg copy in progress
    static constexpr uint8_t kMeasuringBit  = 0x08; // bit 3: 1 while a conversion is actually running
    static constexpr uint8_t kRegReset      = 0xE0;
    static constexpr uint8_t kResetCmd      = 0xB6; // only value the chip acts on; anything else is ignored
    static constexpr uint8_t kRegConfig     = 0xF5;
    static constexpr uint8_t kConfigValue   = 0x00; // t_sb=000 (n/a in forced mode), filter=000 (off), spi3w_en=0

    // ctrl_meas = osrs_t(x1)=001, osrs_p(x1)=001, mode(forced)=01 -> 0b001_001_01
    static constexpr uint8_t kCtrlMeasForced = 0x25;

    // ---- power-on sequence: CheckDevice -> Resetting -> ResetSettle -> WaitImUpdate -> Ready ----
    // Runs once, entirely ahead of anything SensorBase knows about.
    // Calibration + device configuration now happen inside SensorBase's
    // own Calibrate -> Configure states, once Ready delegates to it —
    // no separate phase needed here for those anymore.
    enum class PowerPhase { CheckDevice, Resetting, ResetSettle, WaitImUpdate, Ready, DeviceNotFound };
    static constexpr uint32_t kResetSettleMs = 150; // conservative margin — datasheet gives
                                                      // no exact post-reset settle time
    PowerPhase powerPhase_; // set explicitly in the constructor — see there
    bool       resetRequested_    = false;
    uint32_t   resetSettleStart_  = 0; // set when entering ResetSettle
    bool       statusReadPending_ = false;
    uint8_t    statusByte_        = 0xFF; // default "still copying" until proven otherwise

    // filled in by loadCalibration(), parsed by onCalibrationRead()
    uint8_t calibRaw_[kCalibLen] = {0};

    // factory calibration coefficients, filled in by onCalibrationRead()
    uint16_t dig_T1_ = 0; int16_t dig_T2_ = 0, dig_T3_ = 0;
    uint16_t dig_P1_ = 0;
    int16_t  dig_P2_ = 0, dig_P3_ = 0, dig_P4_ = 0, dig_P5_ = 0,
             dig_P6_ = 0, dig_P7_ = 0, dig_P8_ = 0, dig_P9_ = 0;

    int32_t tFine_ = 0;            // shared between temp & pressure compensation
    float   lastTempC_ = 0.0f;
    float   lastPressurePa_ = 0.0f;
    float   seaLevelPa_ = 101325.0f; // standard-atmosphere default; see getAltitude()

    static uint16_t le16u(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
    static int16_t  le16s(const uint8_t* p) { return static_cast<int16_t>(p[0] | (p[1] << 8)); }

    // Bosch reference compensation (int32/int64 fixed point, as specified
    // in the datasheet). compensateTemperature() must run before
    // compensatePressure() — pressure depends on tFine_.
    int32_t  compensateTemperature(int32_t adcT);
    uint32_t compensatePressure(int32_t adcP); // returns Q24.8; caller divides by 256 for Pa
};
