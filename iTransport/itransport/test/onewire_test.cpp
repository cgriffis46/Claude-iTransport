// Host test for the 1-Wire transport, OneWireUartTransport, with the
// UART calls faked and a simulated DS18B20 on the wire. No HAL or RTOS
// needed (build as one line):
//
//   g++ -std=c++17 -Wall -Wextra -I../inc onewire_test.cpp
//       ../src/BusTransport.cpp ../src/OneWireUartTransport.cpp -o onewire_test
#include <cstdio>
#include <cstring>
#include <vector>
#include "OneWireUartTransport.h"

static int g_failures = 0;
static void check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failures;
}

// Dallas/Maxim CRC-8 (x^8 + x^5 + x^4 + 1), as a DS18B20 puts in byte 8.
static uint8_t crc8(const uint8_t* p, int n) {
    uint8_t crc = 0;
    for (int i = 0; i < n; ++i) {
        uint8_t b = p[i];
        for (int k = 0; k < 8; ++k) {
            const uint8_t mix = (crc ^ b) & 0x01;
            crc >>= 1;
            if (mix) crc ^= 0x8C;
            b >>= 1;
        }
    }
    return crc;
}

// ---- the wire: what the UART's RX sees for each character TX sends ----
struct SimWire {
    bool present = true;       // a DS18B20 is connected
    bool stuckLow = false;     // line shorted to ground
    bool corrupt = false;      // something else disturbing the line

    enum { Idle, RomCommand, FunctionCommand, Sending } state = Idle;
    uint8_t shift = 0; int bits = 0; int sendBit = 0;
    int conversions = 0;
    uint8_t scratchpad[9] = {0x91, 0x01, 0x4B, 0x46, 0x7F, 0xFF, 0x0C, 0x10, 0x00}; // 0x0191 = 25.0625 C

    SimWire() { scratchpad[8] = crc8(scratchpad, 8); }

    uint8_t slot(uint32_t baud, uint8_t tx) {
        if (stuckLow) return 0x00;
        if (baud == 9600) {                                  // reset pulse
            if (!present) return tx;
            state = RomCommand; shift = 0; bits = 0;
            return 0xE0;                                     // presence pulse eats into the high half
        }
        if (!present) return tx;                             // nothing there: plain echo
        if (state == Sending) {                              // device answers a read slot
            const bool one = (scratchpad[sendBit / 8] >> (sendBit % 8)) & 1;
            if (++sendBit >= 72) state = Idle;
            return one ? 0xFF : 0xFC;                        // a 0 holds the line low a little longer
        }
        if (state == RomCommand || state == FunctionCommand) {  // device listens to a write slot
            if (tx == 0xFF) shift |= static_cast<uint8_t>(1u << bits);
            if (++bits == 8) {
                if (state == RomCommand) state = (shift == 0xCC) ? FunctionCommand : Idle;
                else if (shift == 0x44) { ++conversions; state = Idle; }
                else if (shift == 0xBE) { state = Sending; sendBit = 0; }
                else state = Idle;
                shift = 0; bits = 0;
            }
        }
        return corrupt ? static_cast<uint8_t>(tx ^ 0x10) : tx;
    }
};

// ---- OneWireUartTransport with the UART calls faked ----
static int g_uart; // stands in for &huart1
class FakeOneWire : public OneWireUartTransport {
public:
    explicit FakeOneWire(SimWire& w) : OneWireUartTransport(&g_uart, nullptr), wire(w) {}

    std::vector<uint8_t> sent;        // characters of the last transmit
    uint32_t baud = 0;
    int baudChanges = 0, aborts = 0, mutexHeld = 0;
    bool receiveArmedFirst = false;
    bool refuseTransmit = false;

    // The three UART interrupts, fired by the test.
    void txIrq()  { OneWireUartTransport::onUartTxComplete(&g_uart); }
    void rxIrq()  { OneWireUartTransport::onUartRxComplete(&g_uart); }
    void errIrq() { OneWireUartTransport::onUartError(&g_uart); }
    void finish() { rxIrq(); txIrq(); }

protected:
    bool halSetBaud(uint32_t b) override { baud = b; ++baudChanges; return true; }
    bool halReceive(uint8_t* r, uint16_t) override { rx = r; receiveArmedFirst = true; return true; }
    bool halTransmit(uint8_t* t, uint16_t n) override {
        if (refuseTransmit) return false;
        sent.assign(t, t + n);
        for (uint16_t i = 0; i < n; ++i) rx[i] = wire.slot(baud, t[i]);  // what comes back on the line
        return true;
    }
    void halAbort() override { ++aborts; }
    bool ObtainMutex(uint32_t) override { ++mutexHeld; return true; }
    void ReleaseMutex() override { --mutexHeld; }

private:
    SimWire& wire;
    uint8_t* rx = nullptr;
};

int main() {
    using Bytes = std::vector<uint8_t>;

    std::printf("reset(): the reset pulse and presence detect\n");
    {
        SimWire wire; FakeOneWire bus(wire);
        check(!bus.presence(), "no presence claimed before a reset");
        check(bus.reset(), "issued");
        check(bus.baud == 9600 && bus.sent == Bytes({0xF0}), "one character 0xF0 at 9600 baud");
        check(bus.receiveArmedFirst, "receive armed before the transmit");
        check(bus.isBusy() && bus.mutexHeld == 1, "busy, holding the bus");
        bus.txIrq();
        check(bus.isBusy(), "still busy after transmit-complete alone");
        bus.rxIrq();
        check(!bus.isBusy() && !bus.lastOpFailed() && bus.mutexHeld == 0, "done once receive-complete has arrived too");
        check(bus.presence(), "device answered: presence");
    }
    {
        SimWire wire; wire.present = false; FakeOneWire bus(wire);
        bus.reset(); bus.finish();
        check(!bus.isBusy() && !bus.lastOpFailed() && !bus.presence(), "nothing connected: no presence, and not reported as a bus fault");
    }
    {
        SimWire wire; wire.stuckLow = true; FakeOneWire bus(wire);
        bus.reset(); bus.finish();
        check(!bus.isBusy() && bus.lastOpFailed() && !bus.presence(), "line held low: reported as a bus fault");
    }

    std::printf("writeBytes(): one character per bit, least significant bit first\n");
    {
        SimWire wire; FakeOneWire bus(wire);
        bus.reset(); bus.finish(); bus.isBusy();
        { uint8_t cmd[2] = {0xCC, 0x44};   // Skip ROM, Convert T — built on the stack
          check(bus.writeBytes(cmd, 2), "issued"); }
        check(bus.baud == 115200 && bus.sent.size() == 16, "16 characters at 115200 baud");
        check(bus.sent == Bytes({0x00,0x00,0xFF,0xFF,0x00,0x00,0xFF,0xFF,     // 0xCC
                                 0x00,0x00,0xFF,0x00,0x00,0x00,0xFF,0x00}),   // 0x44
              "0xCC then 0x44 as write-0 / write-1 slots");
        bus.rxIrq();
        check(bus.isBusy(), "still busy after receive-complete alone");
        bus.txIrq();
        check(!bus.isBusy() && !bus.lastOpFailed(), "done, read back as sent");
        check(wire.conversions == 1, "the device saw Skip ROM + Convert T");
    }
    {
        SimWire wire; FakeOneWire bus(wire);
        bus.reset(); bus.finish(); bus.isBusy();
        wire.corrupt = true;
        uint8_t cmd[1] = {0xCC};
        bus.writeBytes(cmd, 1); bus.finish();
        check(!bus.isBusy() && bus.lastOpFailed(), "a write that doesn't read back as sent is a bus fault");
    }

    std::printf("readBytes(): read slots decoded into the caller's buffer\n");
    {
        SimWire wire; FakeOneWire bus(wire);
        uint8_t cmd[2] = {0xCC, 0xBE};     // Skip ROM, Read Scratchpad
        bus.reset(); bus.finish(); bus.isBusy();
        bus.writeBytes(cmd, 2); bus.finish(); bus.isBusy();
        uint8_t pad[9] = {0};
        check(bus.readBytes(pad, 9), "issued");
        bool allOnes = bus.sent.size() == 72;
        for (uint8_t c : bus.sent) if (c != 0xFF) allOnes = false;
        check(allOnes, "72 read slots (0xFF)");
        check(pad[0] == 0, "nothing in the caller's buffer until it lands");
        bus.finish();
        check(!bus.isBusy() && !bus.lastOpFailed(), "done");
        check(std::memcmp(pad, wire.scratchpad, 9) == 0, "scratchpad bytes match what the device sent");
        check(crc8(pad, 8) == pad[8], "and its CRC checks");
        const int16_t raw = static_cast<int16_t>(pad[0] | (pad[1] << 8));
        check(raw / 16.0f == 25.0625f, "temperature decodes to 25.0625 C");
    }

    std::printf("baud rate is only changed when it has to be\n");
    {
        SimWire wire; FakeOneWire bus(wire);
        uint8_t cmd[2] = {0xCC, 0xBE}, pad[9];
        bus.reset(); bus.finish(); bus.isBusy();
        bus.writeBytes(cmd, 2); bus.finish(); bus.isBusy();
        bus.readBytes(pad, 9); bus.finish(); bus.isBusy();
        check(bus.baudChanges == 2, "reset, write, read: two changes (to 9600, then to 115200)");
        bus.reset(); bus.finish(); bus.isBusy();
        check(bus.baudChanges == 3 && bus.baud == 9600, "back to 9600 for the next reset");
    }

    std::printf("limits and refusals\n");
    {
        SimWire wire; FakeOneWire bus(wire);
        uint8_t big[20] = {0};
        check(!bus.writeBytes(big, 17) && !bus.readBytes(big, 17), "more than kMaxLen (16) bytes is refused");
        check(!bus.writeBytes(big, 0) && !bus.readBytes(big, 0) && !bus.writeBytes(nullptr, 1) && !bus.readBytes(nullptr, 1), "zero length and null buffer are refused");
        check(bus.mutexHeld == 0, "none of those left the bus held");
        check(bus.writeBytes(big, 16), "exactly 16 is accepted");
        check(!bus.reset() && !bus.writeBytes(big, 1) && !bus.readBytes(big, 1), "a second operation while one is in flight is refused");
        bus.finish(); bus.isBusy();
        bus.refuseTransmit = true;
        check(!bus.reset() && !bus.isBusy() && bus.mutexHeld == 0 && bus.aborts == 1, "UART refusing to transmit: receive half cancelled, bus released");
    }

    std::printf("UART error during an operation\n");
    {
        SimWire wire; FakeOneWire bus(wire);
        uint8_t pad[9];
        bus.reset(); bus.finish(); bus.isBusy();
        bus.readBytes(pad, 9);
        bus.txIrq();
        bus.errIrq();                       // overrun: the receive will never complete
        check(!bus.isBusy() && bus.lastOpFailed(), "ends the operation as failed instead of leaving it busy for ever");
        check(bus.aborts == 1 && bus.mutexHeld == 0, "UART aborted and bus released");
        check(bus.reset(), "and the bus is usable again");
        bus.finish();
        check(!bus.isBusy() && !bus.lastOpFailed() && bus.presence(), "next reset works normally");
    }

    std::printf("a whole DS18B20 conversation\n");
    {
        SimWire wire; FakeOneWire bus(wire);
        auto run = [&](bool issued) { if (!issued) return false; bus.finish(); return !bus.isBusy() && !bus.lastOpFailed(); };
        const uint8_t convert[2] = {0xCC, 0x44}, readPad[2] = {0xCC, 0xBE};
        uint8_t pad[9] = {0};
        bool ok = run(bus.reset()) && bus.presence()
               && run(bus.writeBytes(convert, 2))
               // ... the driver waits out the conversion time here ...
               && run(bus.reset()) && bus.presence()
               && run(bus.writeBytes(readPad, 2))
               && run(bus.readBytes(pad, 9));
        check(ok && wire.conversions == 1, "reset, CC 44, reset, CC BE, read 9 bytes");
        check(crc8(pad, 8) == pad[8] && (pad[0] | (pad[1] << 8)) == 0x0191, "scratchpad arrives intact");
    }

    std::printf("\n%s\n", g_failures ? "FAILED" : "all passed");
    return g_failures ? 1 : 0;
}
