// Host test for LoRaPhy and rfm95<TTransport> (and xrfm95), against
// simulated SX1276 radios sharing an air (sim/SimSX1276.h).
//
// g++ -std=c++17 -fno-exceptions -fno-rtti -Wall -Wextra -I../rfm95/inc -Isim -Istub -I../../isensor/inc -I../../iTransport/itransport/inc rfm95_test.cpp ../../iTransport/itransport/src/BusTransport.cpp ../../iTransport/itransport/src/SPITransport.cpp ../../iTransport/itransport/src/DebugLog.cpp -o rfm95_test
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>
#include "SimSX1276.h"
#include "xrfm95.h"   // pulls in rfm95.h

static int g_checks = 0, g_failures = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { ++g_failures; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

using sim::Sx1276;
typedef rfm95<sim::Bus> Radio;

static lora::Config cfg915(uint8_t sf = 7) {
	lora::Config c = lora::defaultConfig(915000000u);
	c.sf = sf;
	return c;
}

// A radio on the air, with its driver.
template <typename TDrv = Radio, typename TBus = sim::Bus>
struct Node {
	Sx1276 chip;
	std::unique_ptr<TDrv> drv;
	uint32_t irqLatencyMs = 0;   // after a DIO interrupt, main() waits this long (a busy thread)
	bool held = false;
	uint32_t holdUntil = 0;
	void irq() { if (irqLatencyMs) { held = true; holdUntil = chip.now + irqLatencyMs; } }
	explicit Node(sim::Air& air, rfm95_param_t p = rfm95_default_param()) {
		air.add(chip);
		drv.reset(new TDrv(p, chip));
		if (p.dio0Interrupt) chip.dio0 = [this]() { drv->onDio0(chip.now); irq(); };
		if (p.dio1Interrupt) chip.dio1 = [this]() { drv->onDio1(chip.now); irq(); };
	}
};

// Everything advances one ms at a time: the air, the chips, then each
// driver's main() several times, as a real loop calls it again at once
// (an SPI transfer takes microseconds; the bus here needs a poll or two).
struct World {
	sim::Air air;
	uint32_t t = 0;
	std::vector<std::function<void(uint32_t)>> parts;
	void run(uint32_t ms) {
		for (uint32_t i = 0; i < ms; ++i, ++t) {
			air.tick(t);
			for (auto& p : parts) p(t);
		}
	}
	template <typename N> void add(N& n) {
		parts.push_back([&n](uint32_t now) { n.chip.tick(now);
			if (n.held && (int32_t)(now - n.holdUntil) < 0) return;
			n.held = false;
			for (int i = 0; i < 20; ++i) n.drv->main(now);
		});
	}
};

template <typename N> static bool startUp(World& w, N& n) {
	for (int i = 0; i < 200 && !n.drv->ready(); ++i) w.run(1);
	return n.drv->ready();
}

// ---- LoRaPhy ----

static void testPhy() {
	// 915 MHz: the well-known FRF 0xE4C000; 902.3 MHz (US915 channel 0).
	CHECK(lora::frf(915000000u) == 0xE4C000u);
	CHECK(lora::frf(902300000u) == 0xE19333u);   // 902300000 * 2^19 / 32e6 = 14783283.2
	CHECK(lora::freqFromFrf(0xE4C000u) == 915000000u);
	// Time on air, against Semtech's formula worked by hand:
	// SF7/125k, CR 4/5, preamble 8, 10 bytes, CRC: 28 + 12.25 symbols of 1.024 ms.
	lora::Config c = cfg915();
	CHECK(lora::timeOnAirUs(c, 10) == 41216);
	// SF12/125k (LDRO on), 13 bytes (an empty LoRaWAN uplink), CRC:
	// num = 104+16-48+20+8 = 100, den 4*10 = 40 -> 3 * 5 = 15, +8+12 = 35 symbols;
	// (4*35+1) * 2^10 / 125000 = 1155.072 ms.
	c.sf = 12;
	CHECK(lora::timeOnAirUs(c, 13) == 1155072);
	CHECK(lora::lowDataRateOptimize(11, lora::Bw::Bw125k) && lora::lowDataRateOptimize(12, lora::Bw::Bw250k));
	CHECK(!lora::lowDataRateOptimize(10, lora::Bw::Bw125k) && !lora::lowDataRateOptimize(12, lora::Bw::Bw500k));
	CHECK(lora::symbolUs(7, lora::Bw::Bw125k) == 1024);
	// RSSI and SNR (Semtech's conversion, HF port).
	CHECK(lora::packetSnrDb(40) == 10 && lora::packetSnrDb(0xF8) == -2 && lora::packetSnrDb(0xF6) == -2);
	CHECK(lora::packetRssiDbm(100, 40, 915000000u) == -51);
	CHECK(lora::packetRssiDbm(100, 0xF8, 915000000u) == -53);   // SNR -2 is added when negative
	CHECK(lora::packetRssiDbm(100, 40, 433000000u) == -58);     // LF port offset
	lora::Config bad = cfg915(13);
	CHECK(!lora::valid(bad) && lora::valid(cfg915()));
}

// ---- startup ----

static void testStartup() {
	World w;
	Node<> a(w.air);
	w.add(a);
	CHECK(startUp(w, a));
	CHECK(a.chip.lora() && a.chip.mode() == sx1276::kOpStandby);
	CHECK(a.chip.reg[sx1276::kRegSyncWord] == sx1276::kSyncWordLoRaWan);
	CHECK(a.chip.reg[sx1276::kRegLna] == 0x23 && a.chip.reg[sx1276::kRegFifoTxBaseAddr] == 0 && a.chip.reg[sx1276::kRegFifoRxBaseAddr] == 0);
	rfm95_event_t e;
	CHECK(!a.drv->takeEvent(&e, nullptr, 0));

	// A wrong chip: a fault, then it tries again.
	World w2;
	Node<> b(w2.air);
	b.chip.version = 0x22;
	w2.add(b);
	w2.run(50);
	CHECK(!b.drv->ready() && b.drv->stats().faults == 1);
	CHECK(b.drv->takeEvent(&e, nullptr, 0) && e.kind == rfm95_ev_fault);
	b.chip.version = sx1276::kVersion;   // the right chip after all
	w2.run(1100);
	CHECK(b.drv->ready());

	// Nothing there: reads all zero.
	World w3;
	Node<> c(w3.air);
	c.chip.present = false;
	w3.add(c);
	w3.run(50);
	CHECK(!c.drv->ready() && c.drv->stats().faults >= 1);
}

// ---- transmit ----

static void testTransmit() {
	World w;
	Node<> a(w.air);
	w.add(a);
	CHECK(startUp(w, a));
	const uint8_t msg[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
	lora::Config c = cfg915();
	CHECK(a.drv->transmit(c, msg, 10));
	CHECK(!a.drv->transmit(c, msg, 10));   // one at a time
	const uint32_t t0 = w.t;
	rfm95_event_t e;
	int ms = 0;
	while (!a.drv->takeEvent(&e, nullptr, 0) && ms < 200) { w.run(1); ++ms; }
	CHECK(e.kind == rfm95_ev_tx_done);
	CHECK(a.chip.sent.size() == 1);
	const Sx1276::Sent& s = a.chip.sent[0];
	CHECK(s.data == std::vector<uint8_t>(msg, msg + 10));
	CHECK(s.freqHz == 915000000u && s.sf == 7 && s.bw == 7 && s.crc && !s.invertIq);
	CHECK(s.paConfig == (0x80 | 12) && s.paDac == 0x84);   // 14 dBm = PA_BOOST | (14 - 2)
	CHECK(a.chip.reg[sx1276::kRegOcp] == 0x2B);             // 100 mA
	// TX done reported within a poll of the end of the packet (41.2 ms on air).
	CHECK(e.ticks >= s.end && e.ticks <= s.end + 3 && s.end - s.start == 42);
	CHECK(e.ticks - t0 < 60);
	CHECK(a.drv->ready() && a.drv->stats().txDone == 1);

	// +20 dBm: the high power DAC, PaConfig = 20 - 5, 140 mA.
	c.powerDbm = 20;
	CHECK(a.drv->transmit(c, msg, 3));
	w.run(100);
	CHECK(a.chip.sent.back().paConfig == (0x80 | 15) && a.chip.sent.back().paDac == 0x87);
	CHECK(a.chip.reg[sx1276::kRegOcp] == 0x31);
	// Held to param.maxPowerDbm.
	rfm95_param_t p = rfm95_default_param();
	p.maxPowerDbm = 10;
	World w2;
	Node<> b(w2.air, p);
	w2.add(b);
	CHECK(startUp(w2, b));
	CHECK(b.drv->transmit(c, msg, 3));
	w2.run(100);
	CHECK(b.chip.sent.back().paConfig == (0x80 | 8) && b.chip.sent.back().paDac == 0x84);
	// Bad requests.
	lora::Config bad = c;
	bad.sf = 6;
	CHECK(!a.drv->transmit(bad, msg, 3));
	CHECK(!a.drv->transmit(c, msg, 0) && !a.drv->transmit(c, nullptr, 3));
}

// ---- one radio to another ----

static void testLink() {
	World w;
	Node<> a(w.air), b(w.air);
	w.add(a);
	w.add(b);
	CHECK(startUp(w, a) && startUp(w, b));
	// 200 bytes: seven FIFO pieces each way.
	std::vector<uint8_t> msg(200);
	for (size_t i = 0; i < msg.size(); ++i) msg[i] = (uint8_t)(i * 7 + 3);
	CHECK(b.drv->receive(cfg915(), 0));   // continuous
	w.run(5);
	CHECK(b.drv->receivingContinuous());
	CHECK(a.drv->transmit(cfg915(), msg.data(), (uint8_t)msg.size()));
	w.run(400);
	rfm95_event_t e;
	uint8_t buf[255];
	CHECK(b.drv->takeEvent(&e, buf, sizeof buf) && e.kind == rfm95_ev_rx_done);
	CHECK(e.len == 200 && std::memcmp(buf, msg.data(), 200) == 0);
	CHECK(e.rssiDbm == -51 && e.snrDb == 10);
	// Still listening: a second packet.
	const uint8_t two[2] = {0xAB, 0xCD};
	CHECK(a.drv->transmit(cfg915(), two, 2));
	w.run(100);
	CHECK(b.drv->takeEvent(&e, buf, sizeof buf) && e.kind == rfm95_ev_rx_done && e.len == 2 && buf[0] == 0xAB);
	// standby() ends it.
	CHECK(b.drv->standby());
	w.run(10);
	CHECK(b.drv->ready() && !b.chip.inRx());
	CHECK(a.drv->transmit(cfg915(), two, 2));
	w.run(100);
	CHECK(!b.drv->takeEvent(&e, buf, sizeof buf));
	// The FIFO pieces' edges: 1, 32, 33 and 255 bytes.
	CHECK(b.drv->receive(cfg915(), 0));
	w.run(5);
	for (uint8_t len : {1, 32, 33, 255}) {
		std::vector<uint8_t> m(len);
		for (size_t i = 0; i < m.size(); ++i) m[i] = (uint8_t)(i * 13 + len);
		CHECK(a.drv->transmit(cfg915(), m.data(), len));
		w.run(500);
		CHECK(b.drv->takeEvent(&e, buf, sizeof buf) && e.kind == rfm95_ev_rx_done && e.len == len &&
		      std::memcmp(buf, m.data(), len) == 0);
	}
	// The longest op list: 255 bytes at 500 kHz (two errata writes more).
	CHECK(b.drv->standby());
	w.run(5);
	lora::Config wide = cfg915(8);
	wide.bw = lora::Bw::Bw500k;
	CHECK(b.drv->receive(wide, 0));
	w.run(5);
	std::vector<uint8_t> big(255, 0x5A);
	CHECK(a.drv->transmit(wide, big.data(), 255));
	w.run(300);
	CHECK(b.drv->takeEvent(&e, buf, sizeof buf) && e.kind == rfm95_ev_rx_done && e.len == 255 &&
	      std::memcmp(buf, big.data(), 255) == 0);
}

// LoRaWAN's downlinks: sent with I/Q inverted, received inverted.
static void testIqInversion() {
	World w;
	Node<> gw(w.air), dev(w.air);
	w.add(gw);
	w.add(dev);
	CHECK(startUp(w, gw) && startUp(w, dev));
	lora::Config down = cfg915();
	down.invertIq = true;
	down.crc = false;
	const uint8_t msg[5] = {9, 8, 7, 6, 5};
	rfm95_event_t e;
	uint8_t buf[16];
	// A receiver not inverted hears nothing.
	CHECK(dev.drv->receive(cfg915(), 0));
	w.run(5);
	CHECK(gw.drv->transmit(down, msg, 5));
	w.run(100);
	CHECK(!dev.drv->takeEvent(&e, buf, sizeof buf));
	CHECK(gw.chip.sent.back().invertIq && !gw.chip.sent.back().crc);
	// Inverted: it does.
	CHECK(dev.drv->standby());
	w.run(5);
	CHECK(dev.drv->receive(down, 0));
	w.run(5);
	CHECK(dev.chip.reg[sx1276::kRegInvertIq] == 0x67 && dev.chip.reg[sx1276::kRegInvertIq2] == 0x19);
	CHECK(gw.drv->transmit(down, msg, 5));
	w.run(100);
	CHECK(dev.drv->takeEvent(&e, buf, sizeof buf) && e.kind == rfm95_ev_rx_done && e.len == 5 && buf[4] == 5);
}

static void testMismatches() {
	World w;
	Node<> a(w.air), b(w.air);
	w.add(a);
	w.add(b);
	CHECK(startUp(w, a) && startUp(w, b));
	const uint8_t msg[4] = {1, 2, 3, 4};
	rfm95_event_t e;
	uint8_t buf[16];
	// Different SF, different frequency, different bandwidth: nothing.
	lora::Config rx = cfg915(8);
	CHECK(b.drv->receive(rx, 0));
	w.run(5);
	CHECK(a.drv->transmit(cfg915(7), msg, 4));
	w.run(100);
	rx = cfg915();
	rx.freqHz = 915200000u;
	CHECK(b.drv->receive(rx, 0));   // a new request ends the continuous one
	w.run(10);
	CHECK(a.drv->transmit(cfg915(), msg, 4));
	w.run(100);
	rx = cfg915();
	rx.bw = lora::Bw::Bw500k;
	CHECK(b.drv->receive(rx, 0));
	w.run(10);
	CHECK(b.chip.reg[sx1276::kRegHighBwOptimize1] == 0x02 && b.chip.reg[sx1276::kRegHighBwOptimize2] == 0x64);
	CHECK(a.drv->transmit(cfg915(), msg, 4));
	w.run(100);
	CHECK(!b.drv->takeEvent(&e, buf, sizeof buf));
	// SF12 at 125 kHz: low data rate optimisation on both ends, received.
	CHECK(b.drv->receive(cfg915(12), 0));
	w.run(10);
	CHECK(b.chip.ldro());
	CHECK(a.drv->transmit(cfg915(12), msg, 4));
	w.run(1500);
	CHECK(b.drv->takeEvent(&e, buf, sizeof buf) && e.kind == rfm95_ev_rx_done);
}

static void testRxSingleTimeoutAndCrc() {
	World w;
	Node<> a(w.air), b(w.air);
	w.add(a);
	w.add(b);
	CHECK(startUp(w, a) && startUp(w, b));
	rfm95_event_t e;
	uint8_t buf[16];
	// Nothing sent: RxTimeout after 8 symbols (8.2 ms), back to idle.
	const uint32_t t0 = w.t;
	CHECK(b.drv->receive(cfg915(), 8));
	while (!b.drv->takeEvent(&e, buf, sizeof buf) && w.t - t0 < 300) w.run(1);
	CHECK(e.kind == rfm95_ev_rx_timeout);
	CHECK(w.t - t0 < 20);
	CHECK(b.drv->ready() && b.chip.reg[sx1276::kRegSymbTimeoutLsb] == 8);
	// Over 255 symbols: the top two bits go in RegModemConfig2. 300 at SF7
	// is 307 ms.
	const uint32_t t1 = w.t;
	CHECK(b.drv->receive(cfg915(), 300));
	w.run(5);
	CHECK(b.chip.symbTimeout() == 300);
	while (!b.drv->takeEvent(&e, buf, sizeof buf) && w.t - t1 < 600) w.run(1);
	CHECK(e.kind == rfm95_ev_rx_timeout && w.t - t1 >= 307 && w.t - t1 < 320);
	// A packet begun within the window is received whole, even though it
	// ends long after the window.
	const uint8_t msg[20] = {5};
	CHECK(b.drv->receive(cfg915(), 30));
	w.run(3);
	CHECK(a.drv->transmit(cfg915(), msg, 20));
	w.run(150);
	CHECK(b.drv->takeEvent(&e, buf, sizeof buf) && e.kind == rfm95_ev_rx_done && e.len == 20);
	CHECK(b.drv->ready());
	// The same with a long packet: 200 bytes at SF10 are 1.6 s on the air,
	// far beyond the 8-symbol window (no "stuck" fault halfway through).
	std::vector<uint8_t> longMsg(200, 0x3C);
	uint8_t big[255];
	CHECK(b.drv->receive(cfg915(10), 8));
	w.run(2);
	CHECK(a.drv->transmit(cfg915(10), longMsg.data(), 200));
	w.run(2000);
	CHECK(b.drv->takeEvent(&e, big, sizeof big) && e.kind == rfm95_ev_rx_done && e.len == 200);
	CHECK(b.drv->stats().faults == 0 && b.drv->ready());
	// A bad CRC: reported, not delivered.
	b.chip.corruptNext = 1;
	CHECK(b.drv->receive(cfg915(), 30));
	w.run(3);
	CHECK(a.drv->transmit(cfg915(), msg, 20));
	w.run(150);
	CHECK(b.drv->takeEvent(&e, buf, sizeof buf) && e.kind == rfm95_ev_crc_error);
	CHECK(!b.drv->takeEvent(&e, buf, sizeof buf) && b.drv->stats().crcErrors == 1);
}

// channelBusy(): a header heard in RX continuous and no RxDone yet; a
// header that never becomes a packet is forgotten after a 255 byte
// packet's time. busy(): a request waiting or transfers under way.
static void testChannelBusy() {
	World w;
	Node<> a(w.air), b(w.air);
	w.add(a);
	w.add(b);
	CHECK(startUp(w, a) && startUp(w, b));
	CHECK(!b.drv->busy() && !b.drv->channelBusy());
	CHECK(b.drv->receive(cfg915(), 0));
	CHECK(b.drv->busy());                 // queued, not started yet
	w.run(5);
	CHECK(!b.drv->busy() && b.drv->receivingContinuous() && !b.drv->channelBusy());
	std::vector<uint8_t> msg(100, 0x42);
	CHECK(a.drv->transmit(cfg915(), msg.data(), 100));
	const uint32_t t0 = w.t;
	while (!b.drv->channelBusy() && w.t - t0 < 100) w.run(1);
	CHECK(b.drv->channelBusy() && a.chip.txOn);   // header heard, packet still on the air
	rfm95_event_t e;
	uint8_t buf[128];
	while (!b.drv->takeEvent(&e, buf, sizeof buf) && w.t - t0 < 500) w.run(1);
	CHECK(e.kind == rfm95_ev_rx_done && !b.drv->channelBusy());
	// A header with no packet after it (the flag set by hand).
	b.chip.reg[sx1276::kRegIrqFlags] |= sx1276::kIrqValidHeader;
	w.run(5);
	CHECK(b.drv->channelBusy());
	const uint32_t limit = lora::timeOnAirUs(cfg915(), 255) / 1000u + Radio::kRxMarginMs;
	w.run(limit - 20);
	CHECK(b.drv->channelBusy());
	w.run(40);
	CHECK(!b.drv->channelBusy() && !(b.chip.reg[sx1276::kRegIrqFlags] & sx1276::kIrqValidHeader));
	CHECK(b.drv->receivingContinuous());
	// Not in RX continuous: never busy with traffic.
	CHECK(b.drv->standby());
	w.run(5);
	b.chip.reg[sx1276::kRegIrqFlags] |= sx1276::kIrqValidHeader;
	w.run(5);
	CHECK(!b.drv->channelBusy());
}

// With DIO0 wired, TxDone and RxDone are stamped the moment they happen,
// even with a slow poll; and a clock gives the stamps in its own ticks.
struct SimClock : iClock {
	uint32_t* ms;
	explicit SimClock(uint32_t* m) : ms(m) {}
	uint32_t ticksPerSecond() const override { return 32768; }
	uint32_t now() override { return (uint32_t)((uint64_t)*ms * 32768 / 1000); }
};

static void testDioAndClock() {
	World w;
	rfm95_param_t p = rfm95_default_param();
	p.dio0Interrupt = true;
	p.dio1Interrupt = true;
	p.pollMs = 50;
	Node<> a(w.air, p), b(w.air, p);
	a.irqLatencyMs = 4;   // the thread gets to it later: the stamp is still the interrupt's
	b.irqLatencyMs = 3;
	w.add(a);
	w.add(b);
	CHECK(startUp(w, a) && startUp(w, b));
	SimClock clk(&w.t);
	a.drv->setClock(&clk);
	CHECK(a.drv->ticksPerSecond() == 32768);
	const uint8_t msg[10] = {0};
	CHECK(b.drv->receive(cfg915(), 0));
	w.run(5);
	CHECK(a.drv->transmit(cfg915(), msg, 10));
	w.run(120);
	rfm95_event_t e;
	uint8_t buf[16];
	CHECK(a.drv->takeEvent(&e, nullptr, 0) && e.kind == rfm95_ev_tx_done);
	const uint32_t end = a.chip.sent.back().end;
	CHECK(e.ticks == (uint32_t)((uint64_t)end * 32768 / 1000));   // at the end exactly, not at the next poll
	CHECK(b.drv->takeEvent(&e, buf, sizeof buf) && e.kind == rfm95_ev_rx_done && e.ticks == end);
	// RxTimeout through DIO1.
	CHECK(b.drv->standby());
	w.run(5);
	const uint32_t t0 = w.t;
	CHECK(b.drv->receive(cfg915(), 5));
	while (!b.drv->takeEvent(&e, buf, sizeof buf) && w.t - t0 < 300) w.run(1);
	CHECK(e.kind == rfm95_ev_rx_timeout && w.t - t0 < 20);   // well before the 50 ms poll
}

// A transmission that never ends: a fault, and the radio starts again.
static void testStuckTx() {
	World w;
	Node<> a(w.air);
	w.add(a);
	CHECK(startUp(w, a));
	a.chip.txBroken = true;
	const uint8_t msg[10] = {0};
	CHECK(a.drv->transmit(cfg915(), msg, 10));
	w.run(300);
	rfm95_event_t e;
	CHECK(a.drv->takeEvent(&e, nullptr, 0) && e.kind == rfm95_ev_fault);
	CHECK(a.drv->stats().faults == 1 && !a.drv->ready());
	a.chip.txBroken = false;
	a.chip.txOn = false;
	w.run(1200);
	CHECK(a.drv->ready());
	CHECK(a.drv->transmit(cfg915(), msg, 10));
	w.run(100);
	CHECK(a.drv->takeEvent(&e, nullptr, 0) && e.kind == rfm95_ev_tx_done);
}

// The bus stops answering in the middle: a fault, then it starts again.
static void testBusStuck() {
	World w;
	Node<> a(w.air);
	w.add(a);
	CHECK(startUp(w, a));
	a.chip.stuck = true;
	const uint8_t msg[10] = {0};
	CHECK(a.drv->transmit(cfg915(), msg, 10));
	w.run(300);
	CHECK(a.drv->stats().faults >= 1);
	a.chip.stuck = false;
	w.run(1300);
	CHECK(a.drv->ready());
}

// More packets than event slots, none taken: the extra one is counted.
static void testEventOverflow() {
	World w;
	Node<> a(w.air), b(w.air);
	w.add(a);
	w.add(b);
	CHECK(startUp(w, a) && startUp(w, b));
	CHECK(b.drv->receive(cfg915(), 0));
	w.run(5);
	const uint8_t msg[3] = {1, 2, 3};
	for (int i = 0; i < 3; ++i) {
		CHECK(a.drv->transmit(cfg915(), msg, 3));
		w.run(60);
		rfm95_event_t e;
		CHECK(a.drv->takeEvent(&e, nullptr, 0));
	}
	CHECK(b.drv->stats().rxDone == 3 && b.drv->stats().eventsDropped == 1);
}

// The real SPITransport underneath: write-high addresses, 32 byte pieces.
static void testRealSpi() {
	World w;
	Node<rfm95<sim::Spi>, sim::Spi> a(w.air), b(w.air);
	w.add(a);
	w.add(b);
	CHECK(startUp(w, a) && startUp(w, b));
	CHECK(a.chip.lora() && a.chip.reg[sx1276::kRegSyncWord] == 0x34);
	std::vector<uint8_t> msg(100);
	for (size_t i = 0; i < msg.size(); ++i) msg[i] = (uint8_t)(255 - i);
	CHECK(b.drv->receive(cfg915(), 0));
	w.run(5);
	CHECK(a.drv->transmit(cfg915(), msg.data(), 100));
	w.run(300);
	rfm95_event_t e;
	uint8_t buf[255];
	CHECK(b.drv->takeEvent(&e, buf, sizeof buf) && e.kind == rfm95_ev_rx_done && e.len == 100 && std::memcmp(buf, msg.data(), 100) == 0);
}

static void testTickRollover() {
	World w;
	w.t = 0xFFFFFF00u;
	Node<> a(w.air), b(w.air);
	w.add(a);
	w.add(b);
	CHECK(startUp(w, a) && startUp(w, b));
	const uint8_t msg[30] = {7};
	CHECK(b.drv->receive(cfg915(), 0));
	w.run(5);
	CHECK(a.drv->transmit(cfg915(), msg, 30));
	w.run(300);   // across the wrap
	rfm95_event_t e;
	uint8_t buf[64];
	CHECK(a.drv->takeEvent(&e, nullptr, 0) && e.kind == rfm95_ev_tx_done);
	CHECK(b.drv->takeEvent(&e, buf, sizeof buf) && e.kind == rfm95_ev_rx_done && e.len == 30);
	CHECK(a.drv->stats().faults == 0 && b.drv->stats().faults == 0);
}

// xrfm95: the thread sleeps, and DIO0 wakes it.
static void testRtosVariant() {
	g_rtos = SimRtos();
	int token = 0;
	g_rtos.current = &token;
	World w;
	rfm95_param_t p = rfm95_default_param();
	p.dio0Interrupt = true;
	p.pollMs = 20;
	Node<xrfm95<sim::Bus>> a(w.air, p);
	w.add(a);
	CHECK(startUp(w, a));
	const uint8_t msg[10] = {0};
	CHECK(a.drv->transmit(cfg915(), msg, 10));
	w.run(100);
	rfm95_event_t e;
	CHECK(a.drv->takeEvent(&e, nullptr, 0) && e.kind == rfm95_ev_tx_done);
	CHECK(g_rtos.sets > 0 && g_rtos.blocks > 0);
	CHECK(g_rtos.lastTimeout <= Radio::kIdleSleepMs);
}

int main() {
	testPhy();
	testStartup();
	testTransmit();
	testLink();
	testIqInversion();
	testMismatches();
	testRxSingleTimeoutAndCrc();
	testChannelBusy();
	testDioAndClock();
	testStuckTx();
	testBusStuck();
	testEventOverflow();
	testRealSpi();
	testTickRollover();
	testRtosVariant();
	std::printf("rfm95_test: %d checks, %d failures\n", g_checks, g_failures);
	return g_failures ? 1 : 0;
}
