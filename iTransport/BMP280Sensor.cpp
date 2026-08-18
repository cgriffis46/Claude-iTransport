#include "BMP280Sensor.h"
#include <cmath>

// Datasheet: writing 0xB6 here triggers "the complete power-on-reset
// procedure" and reloads calibration data from NVM. Any other value
// is ignored by the chip, so this method has exactly one thing to do.
// Non-blocking: this only *issues* the write; the caller (main()
// below) polls transport_.isBusy() to find out when it lands.
bool BMP280Sensor::reset() {
    return transport_.writeReg(kRegReset, kResetCmd);
}

float BMP280Sensor::getPressure() const {
    return lastPressurePa();
}

float BMP280Sensor::getTemperature() const {
    return lastTemperatureC();
}

// International barometric formula (US Standard Atmosphere
// approximation) — the same one used in Bosch's own BMP280 reference
// code and most consumer altimeter implementations. Accuracy depends
// entirely on how close seaLevelPa_ is to the *actual* local
// sea-level-equivalent pressure right now, which genuinely varies day
// to day with weather (roughly 980-1050 hPa) — the 101325 Pa default
// is a standard reference point, not a live value. For anything
// beyond a rough estimate, call setSeaLevelReferencePa() with a real
// figure: either a known-altitude calibration (take a pressure
// reading at a location of known altitude, solve for seaLevelPa_) or
// a current QNH value from a weather service.
float BMP280Sensor::getAltitude() const {
    return 44330.0f * (1.0f - std::pow(lastPressurePa() / seaLevelPa_, 1.0f / 5.255f));
}

// Power-on sequence, entirely ahead of anything SensorBase knows
// about:
//   1. CheckDevice   — confirm something responds on the bus at all
//   2. Resetting     — issue reset(), wait for the write to land
//   3. ResetSettle   — fixed 150ms margin before the chip's registers
//                      are trustworthy again (no exact value in the
//                      datasheet, so this is a conservative guess)
//   4. WaitImUpdate  — poll status until the resulting NVM reload finishes
//   5. Ready         — hand off to SensorBase, which runs its own
//                       Calibrate -> Configure -> Idle -> Trigger -> ...
//                       cycle (BMP280 plugs into Calibrate via
//                       hasCalibrationData()/loadCalibration()/
//                       onCalibrationRead() below)
//
// No RTOS anywhere in this file: every "still waiting" branch calls
// sleep(1), which is SensorBase's no-op default here — the actual
// progress happens because whatever's calling main() (the user's own
// loop()) calls it again. xBMP280 overrides sleep()/startThread() to
// make those the same calls do something real, without changing a
// single line of the state machine itself.
void BMP280Sensor::main(uint32_t nowMs) {
    switch (powerPhase_) {

    case PowerPhase::CheckDevice:
        if (transport_.checkDevice()) {
            powerPhase_ = PowerPhase::Resetting;
        } else {
            powerPhase_ = PowerPhase::DeviceNotFound; // latch — nothing to talk to
        }
        return;

    case PowerPhase::Resetting:
        if (!resetRequested_) {
            if (reset()) {
                resetRequested_ = true;
            } else {
                sleep(1); // transport busy with something else — wait a beat and retry
            }
        } else if (!transport_.isBusy()) {
            resetRequested_ = false;
            if (!transport_.lastOpFailed()) {
                resetSettleStart_ = nowMs;
                powerPhase_ = PowerPhase::ResetSettle; // write landed — give it a moment before polling
            }
            // else: NACKed — resetRequested_ is false again, so reset()
            // fires again on the next call
        } else {
            sleep(1); // still waiting on the reset write to land
        }
        return;

    case PowerPhase::ResetSettle:
        // Plain nowMs-based poll, same idiom as WaitImUpdate below —
        // works correctly whether main() is called from a bare loop()
        // (sleep(1) here is a no-op, so THIS call's timing doesn't
        // matter — only how often main() gets called does) or from
        // xBMP280's own dedicated task (sleep(1) really yields a tick).
        if (nowMs - resetSettleStart_ >= kResetSettleMs) {
            powerPhase_ = PowerPhase::WaitImUpdate;
        } else {
            sleep(1); // waiting for the reset-settle margin to elapse
        }
        return;

    case PowerPhase::WaitImUpdate:
        if (!statusReadPending_) {
            if (transport_.readRegs(kRegStatus, &statusByte_, 1)) {
                statusReadPending_ = true;
            } else {
                sleep(1); // transport busy with something else — wait a beat and retry
            }
        } else if (!transport_.isBusy()) {
            statusReadPending_ = false;
            if (!transport_.lastOpFailed() && (statusByte_ & kImUpdateBit) == 0) {
                powerPhase_ = PowerPhase::Ready; // copy finished — safe to proceed
            } else {
                sleep(1); // still copying (or the read failed) — wait a beat and poll again
            }
        } else {
            sleep(1); // still waiting on the status read to land
        }
        return;

    case PowerPhase::Ready:
        SensorBase::main(nowMs); // Calibrate -> Configure -> Idle -> Trigger -> ...
        return;

    case PowerPhase::DeviceNotFound:
        return; // latched — no further bus traffic; caller should check isDeviceMissing()
    }
}

// Non-blocking: this only *issues* the burst read of the 24-byte trim
// table into calibRaw_. Called from SensorBase's Calibrate state; the
// base class polls transport_.isBusy() to find out when it lands, then
// calls onCalibrationRead() below to parse it.
bool BMP280Sensor::loadCalibration() {
    return transport_.readRegs(kRegCalibStart, calibRaw_, kCalibLen);
}

// Non-blocking: this only *issues* the write. Runs in
// SensorBase::State::Configure, right after Calibrate and before Idle.
bool BMP280Sensor::configureDevice() {
    return transport_.writeReg(kRegConfig, kConfigValue);
}

// Calibration table layout (0x88..0x9F, little-endian 16-bit words):
//   dig_T1 (u16), dig_T2 (s16), dig_T3 (s16),
//   dig_P1 (u16), dig_P2..dig_P9 (s16 each)
// Reads from calibRaw_, which loadCalibration() filled in above.
void BMP280Sensor::onCalibrationRead() {
    dig_T1_ = le16u(calibRaw_ + 0);
    dig_T2_ = le16s(calibRaw_ + 2);
    dig_T3_ = le16s(calibRaw_ + 4);
    dig_P1_ = le16u(calibRaw_ + 6);
    dig_P2_ = le16s(calibRaw_ + 8);
    dig_P3_ = le16s(calibRaw_ + 10);
    dig_P4_ = le16s(calibRaw_ + 12);
    dig_P5_ = le16s(calibRaw_ + 14);
    dig_P6_ = le16s(calibRaw_ + 16);
    dig_P7_ = le16s(calibRaw_ + 18);
    dig_P8_ = le16s(calibRaw_ + 20);
    dig_P9_ = le16s(calibRaw_ + 22);
}

// Returns temperature in hundredths of a degree C (e.g. 2153 = 21.53C).
// Also sets tFine_, which compensatePressure() depends on.
int32_t BMP280Sensor::compensateTemperature(int32_t adcT) {
    int32_t var1 = ((adcT >> 3) - (static_cast<int32_t>(dig_T1_) << 1)) *
                   static_cast<int32_t>(dig_T2_) >> 11;
    int32_t var2 = (((((adcT >> 4) - static_cast<int32_t>(dig_T1_)) *
                       ((adcT >> 4) - static_cast<int32_t>(dig_T1_))) >> 12) *
                    static_cast<int32_t>(dig_T3_)) >> 14;
    tFine_ = var1 + var2;
    return (tFine_ * 5 + 128) >> 8;
}

// Returns pressure in Q24.8 fixed point (divide by 256 for Pa).
// Must be called after compensateTemperature() in the same cycle.
uint32_t BMP280Sensor::compensatePressure(int32_t adcP) {
    int64_t var1 = static_cast<int64_t>(tFine_) - 128000;
    int64_t var2 = var1 * var1 * static_cast<int64_t>(dig_P6_);
    var2 += (var1 * static_cast<int64_t>(dig_P5_)) << 17;
    var2 += static_cast<int64_t>(dig_P4_) << 35;
    var1 = ((var1 * var1 * static_cast<int64_t>(dig_P3_)) >> 8) +
           ((var1 * static_cast<int64_t>(dig_P2_)) << 12);
    var1 = ((static_cast<int64_t>(1) << 47) + var1) * static_cast<int64_t>(dig_P1_) >> 33;

    if (var1 == 0) return 0; // avoid divide-by-zero (datasheet-specified guard)

    int64_t p = 1048576 - adcP;
    p = ((p << 31) - var2) * 3125 / var1;
    int64_t var1b = (static_cast<int64_t>(dig_P9_) * (p >> 13) * (p >> 13)) >> 25;
    int64_t var2b = (static_cast<int64_t>(dig_P8_) * p) >> 19;
    p = ((p + var1b + var2b) >> 8) + (static_cast<int64_t>(dig_P7_) << 4);
    return static_cast<uint32_t>(p);
}

// raw[] from burst read at 0xF7: press_msb, press_lsb, press_xlsb,
// temp_msb, temp_lsb, temp_xlsb. Only the top 4 bits of each xlsb
// byte are valid (20-bit ADC results).
void BMP280Sensor::decode(const uint8_t* raw) {
    int32_t adcP = (static_cast<int32_t>(raw[0]) << 12) |
                   (static_cast<int32_t>(raw[1]) << 4)  | (raw[2] >> 4);
    int32_t adcT = (static_cast<int32_t>(raw[3]) << 12) |
                   (static_cast<int32_t>(raw[4]) << 4)  | (raw[5] >> 4);

    const int32_t tHundredths = compensateTemperature(adcT); // sets tFine_ — must run first
    const uint32_t pQ24_8     = compensatePressure(adcP);

    lastTempC_      = tHundredths / 100.0f;
    lastPressurePa_ = pQ24_8 / 256.0f;
}
