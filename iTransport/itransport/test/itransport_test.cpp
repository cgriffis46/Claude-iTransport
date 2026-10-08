// Host test for itransport's generic bus classes — BusTransport,
// I2CTransport and SPITransport — with only the hardware calls faked.
// No HAL or RTOS needed (build as one line):
//
//   g++ -std=c++17 -Wall -Wextra -I../inc itransport_test.cpp
//       ../src/BusTransport.cpp ../src/I2CTransport.cpp ../src/SPITransport.cpp -o itransport_test
#include <cstdio>
#include <cstring>
#include <vector>
#include "I2CTransport.h"
#include "SPITransport.h"

static int g_failures = 0;
static void check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failures;
}

// ---- I2CTransport with the HAL calls faked ----
// Transfers stay in flight until the test calls complete(), the way a
// real one stays in flight until the bus interrupt fires.
static int g_bus; // stands in for &hi2c1
class FakeI2C : public I2CTransport {
public:
    explicit FakeI2C(void* bus = &g_bus) : I2CTransport(bus, 0x38, nullptr), bus_(bus) {}

    enum Call { None, MemWrite, MemRead, MasterTransmit, MasterReceive };
    Call     last = None;
    uint8_t  reg = 0;
    uint8_t* ptr = nullptr;   // what the HAL was handed
    uint16_t size = 0;
    bool     refuse = false;  // HAL returns an error instead of starting
    int      mutexHeld = 0;
    uint8_t  rxData[8] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};

    void complete(bool failed = false) {
        if (!failed && (last == MemRead || last == MasterReceive)) std::memcpy(ptr, rxData, size);
        BusTransport::onTransferComplete(bus_, failed); // what the I2C interrupt does
    }
    std::vector<uint8_t> sent() const { return std::vector<uint8_t>(ptr, ptr + size); }

protected:
    bool halMemWrite(uint8_t r, uint8_t* p, uint16_t n) override { return note(MemWrite, r, p, n); }
    bool halMemRead(uint8_t r, uint8_t* p, uint16_t n) override { return note(MemRead, r, p, n); }
    bool halMasterTransmit(uint8_t* p, uint16_t n) override { return note(MasterTransmit, 0, p, n); }
    bool halMasterReceive(uint8_t* p, uint16_t n) override { return note(MasterReceive, 0, p, n); }
    bool halIsDeviceReady(uint32_t, uint32_t) override { return true; }
    bool ObtainMutex(uint32_t) override { ++mutexHeld; return true; }
    void ReleaseMutex() override { --mutexHeld; }

private:
    void* bus_;
    bool note(Call c, uint8_t r, uint8_t* p, uint16_t n) {
        if (refuse) return false;
        last = c; reg = r; ptr = p; size = n;
        return true;
    }
};

// ---- SPITransport with the HAL calls faked ----
static int g_spi; // stands in for &hspi1
class FakeSPI : public SPITransport {
public:
    FakeSPI() : SPITransport(&g_spi, nullptr, 0, nullptr) {}

    std::vector<uint8_t> tx;
    bool    wasFullDuplex = false;
    bool    csLow = false;
    int     csFalls = 0, csRises = 0;
    int     mutexHeld = 0;
    bool    csFellWithBusHeld = true; // chip-select only ever went low while holding the bus
    uint8_t* rx = nullptr; uint16_t rxLen = 0;

    void complete(bool failed = false) {
        for (uint16_t i = 0; !failed && rx && i < rxLen; ++i) rx[i] = static_cast<uint8_t>(0xA0 + i);
        BusTransport::onTransferComplete(&g_spi, failed); // what the SPI interrupt does
    }

protected:
    bool halTransmit(uint8_t* t, uint16_t n) override {
        tx.assign(t, t + n); wasFullDuplex = false; rx = nullptr; return true;
    }
    bool halTransmitReceive(uint8_t* t, uint8_t* r, uint16_t n) override {
        tx.assign(t, t + n); wasFullDuplex = true; rx = r; rxLen = n; return true;
    }
    void halCsLow() override { csLow = true; ++csFalls; if (mutexHeld != 1) csFellWithBusHeld = false; }
    void halCsHigh() override { csLow = false; ++csRises; }
    bool ObtainMutex(uint32_t) override { ++mutexHeld; return true; }
    void ReleaseMutex() override { --mutexHeld; }
};

int main() {
    using Bytes = std::vector<uint8_t>;

    std::printf("I2C writeRegs(): several registers in one transaction\n");
    {
        FakeI2C bus;
        uint8_t cfg[3] = {0x27, 0x00, 0xA0};
        check(bus.writeRegs(0xF4, cfg, 3), "issued");
        check(bus.last == FakeI2C::MemWrite && bus.reg == 0xF4 && bus.size == 3, "one Mem_Write of 3 bytes at 0xF4");
        check(bus.ptr != cfg, "HAL was handed the transport's copy, not the caller's buffer");
        std::memset(cfg, 0xEE, sizeof(cfg)); // the caller's local goes out of scope / gets reused
        check(bus.sent() == Bytes({0x27, 0x00, 0xA0}), "bytes still intact after the caller's buffer is overwritten");
        check(bus.isBusy() && bus.mutexHeld == 1, "busy, and holding the bus, until the interrupt");
        bus.complete();
        check(!bus.isBusy() && !bus.lastOpFailed() && bus.mutexHeld == 0, "done and bus released after the interrupt");
    }

    std::printf("I2C writeReg(): unchanged, now a one-byte writeRegs()\n");
    {
        FakeI2C bus;
        check(bus.writeReg(0xE0, 0xB6), "issued");
        check(bus.last == FakeI2C::MemWrite && bus.reg == 0xE0 && bus.sent() == Bytes({0xB6}), "one Mem_Write of 0xB6 at 0xE0");
        bus.complete(); bus.isBusy();
    }

    std::printf("I2C writeBytes(): a command, no register address\n");
    {
        FakeI2C bus;
        { uint8_t cmd[3] = {0xAC, 0x33, 0x00}; // AHT20 trigger measurement, built on the stack
          check(bus.writeBytes(cmd, 3), "issued"); }
        check(bus.last == FakeI2C::MasterTransmit && bus.sent() == Bytes({0xAC, 0x33, 0x00}), "one Master_Transmit of AC 33 00");
        bus.complete(); bus.isBusy();
        uint8_t reset = 0xBA;
        check(bus.writeBytes(&reset, 1) && bus.sent() == Bytes({0xBA}), "single-byte command (soft reset 0xBA)");
        bus.complete(); bus.isBusy();
    }

    std::printf("I2C readBytes(): a plain read, no register address\n");
    {
        FakeI2C bus;
        uint8_t buf[7] = {0};
        check(bus.readBytes(buf, 7), "issued");
        check(bus.last == FakeI2C::MasterReceive && bus.ptr == buf && bus.size == 7, "one Master_Receive of 7 bytes into the caller's buffer");
        bus.complete();
        check(!bus.isBusy() && buf[0] == 0x11 && buf[6] == 0x77, "data is in the caller's buffer once it lands");
    }

    std::printf("I2C readRegs(): unchanged\n");
    {
        FakeI2C bus;
        uint8_t buf[6] = {0};
        check(bus.readRegs(0xF7, buf, 6) && bus.last == FakeI2C::MemRead && bus.reg == 0xF7 && bus.size == 6, "one Mem_Read of 6 bytes at 0xF7");
        bus.complete(); bus.isBusy();
    }

    std::printf("I2C limits and refusals\n");
    {
        FakeI2C bus;
        uint8_t big[40] = {0};
        check(!bus.writeRegs(0x10, big, 33) && !bus.writeBytes(big, 33), "a write over kMaxWriteLen (32) is refused");
        check(bus.writeBytes(big, 32), "exactly 32 is accepted");
        bus.complete(); bus.isBusy();
        check(!bus.writeRegs(0x10, big, 0) && !bus.writeBytes(big, 0) && !bus.readBytes(big, 0), "zero length is refused");
        check(!bus.writeBytes(nullptr, 2) && !bus.readBytes(nullptr, 2), "null buffer is refused");
        check(bus.mutexHeld == 0, "none of those refusals left the bus held");

        uint8_t a[2] = {1, 2}, b[2] = {9, 9};
        bus.writeBytes(a, 2);
        check(!bus.writeBytes(b, 2) && !bus.readBytes(b, 2), "a second transfer while one is in flight is refused");
        check(bus.sent() == Bytes({1, 2}), "and the transfer in flight is not disturbed");
        bus.complete(/*failed=*/true);
        check(!bus.isBusy() && bus.lastOpFailed() && bus.mutexHeld == 0, "a NACK is reported and the bus released");

        bus.refuse = true;
        check(!bus.writeBytes(a, 2) && !bus.isBusy() && bus.mutexHeld == 0, "HAL refusing to start leaves the transport idle and the bus free");
    }

    std::printf("SPI writeRegs() / writeReg(): address with the read bit cleared, then data\n");
    {
        FakeSPI bus;
        uint8_t cfg[2] = {0x27, 0xA0};
        check(bus.writeRegs(0xF4, cfg, 2), "issued");
        check(bus.tx == Bytes({0x74, 0x27, 0xA0}) && !bus.wasFullDuplex, "sent 74 27 A0, transmit only");
        check(bus.csLow && bus.isBusy() && bus.mutexHeld == 1, "chip-select low and bus held while in flight");
        check(bus.csFellWithBusHeld, "the bus was taken before chip-select went low");
        bus.complete();
        check(!bus.isBusy() && !bus.csLow && bus.csFalls == 1 && bus.csRises == 1, "the interrupt ends it: chip-select released once");
        check(bus.mutexHeld == 0, "and the bus released");
        bus.writeReg(0xE0, 0xB6);
        check(bus.tx == Bytes({0x60, 0xB6}), "writeReg() still sends 60 B6");
        bus.complete(); bus.isBusy();
    }

    std::printf("SPI readRegs(): unchanged\n");
    {
        FakeSPI bus;
        uint8_t buf[3] = {0};
        bus.readRegs(0x77, buf, 3);
        check(bus.tx == Bytes({0xF7, 0xFF, 0xFF, 0xFF}) && bus.wasFullDuplex, "sent F7 then three clocking bytes");
        bus.complete(); bus.isBusy();
        check(buf[0] == 0xA1 && buf[2] == 0xA3, "byte clocked in during the address is skipped");
    }

    std::printf("SPI AddressBit::WriteHigh (Semtech SX1231 / RFM69): bit 7 set to write\n");
    {
        FakeSPI bus;
        check(bus.addressBit() == SPITransport::AddressBit::ReadHigh, "ReadHigh by default");
        bus.setAddressBit(SPITransport::AddressBit::WriteHigh);
        uint8_t frf[3] = {0xE3, 0xDA, 0x7C};
        bus.writeRegs(0x07, frf, 3);
        check(bus.tx == Bytes({0x87, 0xE3, 0xDA, 0x7C}) && !bus.wasFullDuplex, "writeRegs(0x07): sent 87 E3 DA 7C");
        bus.complete(); bus.isBusy();
        uint8_t buf[2] = {0};
        bus.readRegs(0x10, buf, 2);
        check(bus.tx == Bytes({0x10, 0xFF, 0xFF}) && bus.wasFullDuplex, "readRegs(0x10): sent 10, bit 7 clear");
        bus.complete(); bus.isBusy();
        check(buf[0] == 0xA1 && buf[1] == 0xA2, "and reads as before");
        bus.readRegs(0x90, buf, 1);
        check(bus.tx[0] == 0x10, "a register number with bit 7 set is still a read");
        bus.complete(); bus.isBusy();
        bus.writeReg(0x01, 0x04);
        check(bus.tx == Bytes({0x81, 0x04}), "writeReg(0x01, 0x04): sent 81 04");
        bus.complete(); bus.isBusy();
    }

    std::printf("SPI writeBytes() / readBytes(): raw, no address byte\n");
    {
        FakeSPI bus;
        uint8_t cmd[2] = {0xBE, 0x01};
        bus.writeBytes(cmd, 2);
        check(bus.tx == Bytes({0xBE, 0x01}) && !bus.wasFullDuplex, "bytes go out untouched (no read bit masking)");
        bus.complete(); bus.isBusy();
        uint8_t buf[4] = {0};
        bus.readBytes(buf, 4);
        check(bus.tx == Bytes({0xFF, 0xFF, 0xFF, 0xFF}) && bus.wasFullDuplex, "four clocking bytes, nothing else");
        bus.complete(); bus.isBusy();
        check(buf[0] == 0xA0 && buf[3] == 0xA3, "all four bytes clocked in are data");
        uint8_t big[40] = {0};
        check(!bus.writeBytes(big, 33) && !bus.writeRegs(0, big, 33) && !bus.readBytes(big, 33) && !bus.writeBytes(big, 0), "length limits enforced");
        check(bus.csFalls == bus.csRises, "refusals never touched chip-select");
    }

    std::printf("SPI transfer that faults\n");
    {
        FakeSPI bus;
        uint8_t buf[3] = {0x5A, 0x5A, 0x5A};
        bus.readRegs(0x77, buf, 3);
        bus.complete(/*failed=*/true); // HAL_SPI_ErrorCallback
        check(!bus.isBusy() && bus.lastOpFailed(), "reported through lastOpFailed()");
        check(!bus.csLow && bus.mutexHeld == 0, "chip-select and bus released all the same");
        check(buf[0] == 0x5A && buf[2] == 0x5A, "caller's buffer left alone");
    }

    std::printf("two devices on one bus, no RTOS (BusTransport's default hooks)\n");
    {
        FakeI2C a, b;
        uint8_t cmd[2] = {0x01, 0x02};
        check(a.writeBytes(cmd, 2), "first device starts a transfer");
        check(!b.writeBytes(cmd, 2) && !b.readBytes(cmd, 2), "second device is refused while it is in flight");
        a.complete();
        check(!a.isBusy() && !a.lastOpFailed(), "first device's completion is still delivered");
        check(b.writeBytes(cmd, 2), "second device can go once the first has landed");
        b.complete(); b.isBusy();

        FakeSPI c, d;
        c.writeBytes(cmd, 2);
        check(!d.writeBytes(cmd, 2) && d.csFalls == 0, "SPI: second device refused, its chip-select never asserted");
        c.complete(); c.isBusy();
    }

    std::printf("a transport destroyed mid-transfer\n");
    {
        uint8_t cmd[1] = {0x01};
        { FakeI2C gone; gone.writeBytes(cmd, 1); }                // destroyed with a transfer in flight
        BusTransport::onTransferComplete(&g_bus, false);           // its interrupt arrives late: must not touch it
        FakeI2C next;
        check(next.writeBytes(cmd, 1), "the bus is usable by the next transport");
        next.complete(); next.isBusy();
    }

    std::printf("more buses than the registry holds\n");
    {
        // g_bus and g_spi already hold two of the eight slots.
        static int handles[7];
        uint8_t cmd[1] = {0x01};
        bool firstSixOk = true;
        for (int i = 0; i < 6; ++i) {
            FakeI2C t(&handles[i]);
            if (!t.writeBytes(cmd, 1)) firstSixOk = false;
            t.complete(); t.isBusy();
        }
        check(firstSixOk, "eight buses in all work");
        FakeI2C ninth(&handles[6]);
        check(!ninth.writeBytes(cmd, 1) && !ninth.isBusy() && ninth.mutexHeld == 0, "a ninth refuses transfers instead of starting one it could never finish");
    }

    std::printf("\n%s\n", g_failures ? "FAILED" : "all passed");
    return g_failures ? 1 : 0;
}
