#pragma once
#include <cstdint>

// ISensorTransport is the seam between "how we talk to a chip" and
// "what the chip means." Nothing above this interface knows whether
// it's I2C, SPI, or a mock for unit tests.
//
// All operations are non-blocking: writeReg()/readRegs() *start* a
// transfer and return immediately. Call isBusy() every tick() to find
// out when it's done, and lastOpFailed() to check the outcome.
class ISensorTransport {
public:
    virtual ~ISensorTransport() = default;

    // Begin writing `value` to register `reg`. Returns false if a
    // transfer is already in flight (caller should retry next tick).
    virtual bool writeReg(uint8_t reg, uint8_t value) = 0;

    // Begin reading `len` bytes starting at register `reg` into `buf`.
    // `buf` must stay valid until isBusy() returns false.
    virtual bool readRegs(uint8_t reg, uint8_t* buf, uint8_t len) = 0;

    // True while the most recent writeReg()/readRegs() is still in flight.
    virtual bool isBusy() const = 0;

    // Valid once isBusy() == false. True if the last transfer NACKed,
    // timed out, or otherwise failed.
    virtual bool lastOpFailed() const = 0;

    // Confirms a device actually responds on the bus, before any
    // register access is attempted. How this is implemented is
    // entirely bus-specific — hence a virtual here rather than in
    // SensorBase. NOTE: unlike everything else in this interface,
    // this is allowed to block briefly; not every bus (or every HAL)
    // offers a non-blocking way to do it.
    virtual bool checkDevice() = 0;
};
