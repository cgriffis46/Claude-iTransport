// Host test for SpiBlockTransport — the header-then-block SPI
// transport a W5500 uses — on its own (BusTransport's polling
// defaults) and under FreeRtosTransport, against the simulated RTOS in
// stub/cmsis_os2.h. Build as one line:
//
//   g++ -std=c++17 -Wall -Wextra -I../inc -I../hw/freertos/inc -Istub spi_block_transport_test.cpp
//       ../src/BusTransport.cpp ../src/SpiBlockTransport.cpp -o spi_block_transport_test
#include <cstdio>
#include <cstring>
#include <vector>
#include "SpiBlockTransport.h"
#include "FreeRtosTransport.h"

static int g_failures = 0;
static void check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failures;
}

static int g_spi1, g_spi2;   // stand in for &hspi1, &hspi2
static int g_mtxSpi1;        // an osMutexNew() result
static int g_taskA;

// Records each hardware call. Nothing completes until the test fires
// the interrupt with irq(), as on the real thing.
template <typename TBase>
class Fake : public TBase {
public:
    Fake(void* bus, void* mutex) : TBase(bus, nullptr, 0, mutex), bus_(bus) {}

    struct Call { bool rx; uint8_t* ptr; uint16_t len; bool csLowAtCall; };
    std::vector<Call> calls;
    bool csLow = false;
    int  refuseCall = -1;          // index of the hal*() call to refuse
    uint8_t fill = 0xA5;           // what a receive "clocks in"

    void irq(bool failed = false) { BusTransport::onTransferComplete(bus_, failed); }
    std::vector<uint8_t> sent(size_t i) const { return {calls[i].ptr, calls[i].ptr + calls[i].len}; }

protected:
    bool halTransmit(uint8_t* tx, uint16_t n) override { return note(false, tx, n); }
    bool halReceive(uint8_t* rx, uint16_t n) override {
        if (!note(true, rx, n)) return false;
        std::memset(rx, fill, n);
        return true;
    }
    void halCsLow() override { csLow = true; }
    void halCsHigh() override { csLow = false; }

private:
    bool note(bool rx, uint8_t* p, uint16_t n) {
        if (static_cast<int>(calls.size()) == refuseCall) { calls.push_back({rx, nullptr, 0, csLow}); return false; }
        calls.push_back({rx, p, n, csLow});
        return true;
    }
    void* bus_;
};

using PlainBlock = Fake<SpiBlockTransport>;
using RtosBlock  = Fake<FreeRtosTransport<SpiBlockTransport>>;

static const uint8_t kHdrRead[3]  = {0x00, 0x39, 0x00};  // W5500: VERSIONR, common block, read
static const uint8_t kHdrWrite[3] = {0x00, 0x09, 0x04};  // W5500: SHAR, common block, write

int main() {
    std::printf("write: header, then the data straight from the caller's buffer\n");
    {
        PlainBlock spi(&g_spi1, nullptr);
        uint8_t mac[6] = {0x02, 0x00, 0x00, 0x12, 0x34, 0x56};
        check(spi.beginWrite(kHdrWrite, 3, mac, 6), "issued");
        check(spi.calls.size() == 1 && !spi.calls[0].rx && spi.sent(0) == std::vector<uint8_t>(kHdrWrite, kHdrWrite + 3),
              "header goes out first, as a transmit");
        check(spi.calls[0].csLowAtCall, "chip-select low before the header");
        check(spi.isBusy(), "busy while the header is in flight");
        spi.irq();
        check(spi.calls.size() == 2 && !spi.calls[1].rx && spi.calls[1].ptr == mac && spi.calls[1].len == 6,
              "header's interrupt starts the data phase, from the caller's buffer");
        check(spi.csLow && spi.isBusy(), "chip-select still low, still busy, between the phases");
        spi.irq();
        check(!spi.isBusy() && !spi.lastOpFailed() && !spi.csLow, "data phase's interrupt finishes it; chip-select raised");
        check(spi.beginWrite(kHdrWrite, 3, mac, 6), "bus free for the next transfer");
        spi.irq();
        spi.irq();
        check(!spi.isBusy(), "which lands too");
    }

    std::printf("read: header, then the data received straight into the caller's buffer\n");
    {
        PlainBlock spi(&g_spi1, nullptr);
        uint8_t buf[2048] = {0};
        check(spi.beginRead(kHdrRead, 3, buf, sizeof buf), "a 2 KB read is issued (no 32-byte limit)");
        spi.irq();
        check(spi.calls.size() == 2 && spi.calls[1].rx && spi.calls[1].ptr == buf && spi.calls[1].len == 2048,
              "data phase is a receive into the caller's buffer");
        spi.irq();
        check(!spi.isBusy() && !spi.lastOpFailed() && buf[0] == 0xA5 && buf[2047] == 0xA5, "landed");
    }

    std::printf("failures and limits\n");
    {
        PlainBlock spi(&g_spi1, nullptr);
        uint8_t buf[4] = {0};
        spi.beginRead(kHdrRead, 3, buf, 4);
        spi.irq(/*failed=*/true);
        check(spi.calls.size() == 1, "a failed header is not followed by a data phase");
        check(!spi.isBusy() && spi.lastOpFailed() && !spi.csLow, "transfer over, failed, chip-select raised");

        spi.calls.clear();
        spi.refuseCall = 1; // the data phase won't start
        spi.beginRead(kHdrRead, 3, buf, 4);
        spi.irq();
        check(!spi.isBusy() && spi.lastOpFailed() && !spi.csLow, "data phase that fails to start ends the transfer, failed");

        spi.calls.clear();
        spi.refuseCall = 0; // the header won't start
        check(!spi.beginRead(kHdrRead, 3, buf, 4) && !spi.csLow, "refused header: not issued, chip-select raised");
        check(!spi.isBusy(), "and nothing left in flight");
        spi.refuseCall = -1;

        spi.calls.clear();
        check(spi.beginWrite(kHdrWrite, 3, nullptr, 0), "header only (len 0) is allowed");
        spi.irq();
        check(spi.calls.size() == 1 && !spi.isBusy() && !spi.lastOpFailed(), "and is one phase");

        spi.calls.clear();
        check(spi.beginRead(nullptr, 0, buf, 4) && spi.calls.size() == 1 && spi.calls[0].rx, "no header: straight to the data");
        spi.irq();
        check(!spi.isBusy(), "and is one phase");

        uint8_t longHdr[9] = {0};
        check(!spi.beginWrite(longHdr, 9, buf, 1), "header over kMaxHeaderLen refused");
        check(!spi.beginWrite(nullptr, 0, nullptr, 0), "nothing at all refused");
        check(!spi.beginRead(kHdrRead, 3, nullptr, 4), "null data with a length refused");
    }

    std::printf("FreeRtosTransport<SpiBlockTransport>: one wake-up per transfer, not per phase\n");
    {
        RtosBlock spi(&g_spi2, &g_mtxSpi1);
        g_rtos.current = &g_taskA;
        g_rtos.sets = 0;
        uint8_t buf[512] = {0};
        check(spi.beginRead(kHdrRead, 3, buf, sizeof buf), "issued");
        check(g_rtos.owner[&g_mtxSpi1] == &g_taskA, "bus mutex held by the issuing thread");
        spi.irq();
        check(g_rtos.sets == 0, "header's interrupt wakes nobody");
        g_rtos.whileBlocked = [&] { spi.irq(); };
        check(!spi.isBusy(), "thread sleeps in isBusy() and is woken by the data phase's interrupt");
        check(g_rtos.sets == 1 && g_rtos.owner[&g_mtxSpi1] == nullptr && !spi.csLow, "woken once; mutex released; deselected");
    }

    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "PASSED", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
