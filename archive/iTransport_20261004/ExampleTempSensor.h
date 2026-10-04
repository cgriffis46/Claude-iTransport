#pragma once
#include "SensorBase.h"

// Stand-in "real sensor" — just enough to prove the pattern end to
// end. Delete this once BME280Sensor/MPU6050Sensor/etc. exist; it's
// scaffolding, not a real driver.
class ExampleTempSensor : public SensorBase {
public:
    explicit ExampleTempSensor(ISensorTransport& transport) : SensorBase(transport) {}

    float lastTempC() const { return lastTempC_; }

protected:
    uint8_t  triggerRegister() const override { return 0xF4; }
    uint8_t  triggerValue()    const override { return 0x01; }
    uint8_t  dataRegister()    const override { return 0xFA; }
    uint8_t  dataLength()      const override { return 3; }
    uint32_t conversionTimeMs() const override { return 50; }

    void decode(const uint8_t* raw) override {
        // placeholder conversion — replace with real datasheet math
        int32_t counts = (raw[0] << 16) | (raw[1] << 8) | raw[2];
        lastTempC_ = static_cast<float>(counts) / 4096.0f;
    }

private:
    float lastTempC_ = 0.0f;
};
