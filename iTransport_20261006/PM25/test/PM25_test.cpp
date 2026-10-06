/*
 * PM25_test.cpp
 *
 * Host test for PM25<TTransport>. No hardware, HAL or RTOS needed.
 * IT is the itransport folder of the safeTransport project:
 *
 *   g++ -std=c++17 -Wall -Wextra -I../inc -Istub -I$IT/inc -I<safeTransport>/isensor/inc \
 *       -I$IT/hw/stm32/inc -I$IT/test/stub_hal PM25_test.cpp $IT/test/stub_hal/fake_hal.cpp \
 *       $IT/hw/stm32/src/Stm32HalUartTransport.cpp $IT/hw/stm32/src/Stm32UartItCallbacks.cpp \
 *       $IT/src/BusTransport.cpp $IT/src/OneWireUartTransport.cpp -o PM25_test
 *
 * Runs the driver on a bare iTransport, and on itransport's real
 * Stm32HalUartTransport over a simulated HAL UART (stub_hal/).
 * stub/cmsis_os2.h is a small simulation of the RTOS calls xPM25 makes.
 */

#include <cstdio>
#include <vector>
#include "Stm32HalUartTransport.h"
#include "xPM25.h"	// pulls in PM25.h

// ---- what the sensor sends ----
struct Sample {
	uint16_t pm10s = 5, pm25s = 12, pm100s = 14, pm10e = 5, pm25e = 11, pm100e = 13;
	uint16_t p03 = 1200, p05 = 340, p10 = 60, p25 = 8, p50 = 2, p100 = 1;
};
static std::vector<uint8_t> makeFrame(const Sample& s) {
	const uint16_t v[14] = {28, s.pm10s, s.pm25s, s.pm100s, s.pm10e, s.pm25e, s.pm100e,
							s.p03, s.p05, s.p10, s.p25, s.p50, s.p100, 0x9700};
	std::vector<uint8_t> f = {0x42, 0x4D};
	for (uint16_t x : v) { f.push_back((uint8_t)(x >> 8)); f.push_back((uint8_t)(x & 0xFF)); }
	uint16_t sum = 0; for (uint8_t b : f) sum = (uint16_t)(sum + b);
	f.push_back((uint8_t)(sum >> 8)); f.push_back((uint8_t)(sum & 0xFF));
	return f;
}

// ---- transport 1: a bare iTransport ----
class FakeStream : public iTransport {
public:
	FakeStream() {}
	explicit FakeStream(int* attachCounter) : attaches(attachCounter) {}
	bool write(const uint8_t*, size_t) override { ++writes(); return false; }
	void setRxSink(iTransportRxSink& s) override { sink() = &s; if (attaches) ++*attaches; }
	// one transport at a time in these tests, so statics are enough to reach it from outside
	static iTransportRxSink*& sink() { static iTransportRxSink* s = nullptr; return s; }
	static int& writes() { static int n = 0; return n; }
private:
	int* attaches = nullptr;
};
static void feed(const std::vector<uint8_t>& bytes) {		// what the UART interrupt does, byte by byte
	for (uint8_t b : bytes) if (FakeStream::sink()) FakeStream::sink()->onByteReceived(b);
}

// ---- harness ----
static int g_failures = 0;
static void check(bool ok, const char* what) {
	std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok) ++g_failures;
}

// Calls main() once per ms from start for ms. Returns frames delivered.
template <typename TSensor>
int run(TSensor& s, uint32_t start, uint32_t ms, PM25_AQI_Data* last = nullptr) {
	int n = 0;
	for (uint32_t i = 0; i < ms; ++i) {
		s.main(start + i);
		if (s.newData()) { PM25_AQI_Data d; s.getData(&d); if (last) *last = d; ++n; }
	}
	return n;
}

int main() {
	const std::vector<uint8_t> good = makeFrame(Sample());

	std::printf("PM25<FakeStream>: a frame a second\n");
	{
		PM25<FakeStream> sensor;
		PM25_AQI_Data d = {};
		check(!sensor.getData(&d) && !sensor.newData(), "getData() says there is nothing before the first frame");
		int n = 0; uint32_t t = 0;
		for (int i = 0; i < 3; ++i) { n += run(sensor, t, 1000, &d); t += 1000; feed(good); }
		n += run(sensor, t, 10, &d);
		check(n == 3, "three frames in, three readings out");
		check(d.framelen == 28 && d.pm10_standard == 5 && d.pm25_standard == 12 && d.pm100_standard == 14, "standard concentrations unpacked");
		check(d.pm10_env == 5 && d.pm25_env == 11 && d.pm100_env == 13, "environmental concentrations unpacked");
		check(d.particles_03um == 1200 && d.particles_05um == 340 && d.particles_10um == 60 && d.particles_25um == 8 && d.particles_50um == 2 && d.particles_100um == 1, "particle counts unpacked");
		check(sensor.getData(&d) && !sensor.newData(), "newData() is false once the reading has been collected");
		check(FakeStream::writes() == 0, "nothing is ever sent to the sensor");
	}

	std::printf("lining up with the frames\n");
	{
		PM25<FakeStream> sensor; run(sensor, 0, 5);
		feed(std::vector<uint8_t>(good.begin() + 12, good.end()));	// reception starts part way through a frame
		check(run(sensor, 5, 5) == 0, "the tail of a frame is not taken for a frame");
		feed(good);
		check(run(sensor, 10, 5) == 1, "the first whole frame after it is received");
	}
	{
		PM25<FakeStream> sensor; run(sensor, 0, 5);
		std::vector<uint8_t> shortFrame = good; shortFrame.erase(shortFrame.begin() + 17);	// one byte lost on the way
		feed(shortFrame);
		check(run(sensor, 5, 5) == 0, "a frame with a byte missing is not accepted");
		feed(good); feed(good);
		const int n = run(sensor, 10, 5);
		feed(good);
		check(n + run(sensor, 15, 5) >= 1, "and reception lines up again within the next two frames");
	}
	{
		PM25<FakeStream> sensor; run(sensor, 0, 5);
		feed({0x00, 0xFF, 0x42, 0x13, 0x42, 0x42});				// noise, including stray start bytes
		feed(good);
		check(run(sensor, 5, 5) == 1, "noise before a frame, stray 0x42s included, does not hide it");
	}
	{
		Sample s; s.p03 = 0x424D; s.p05 = 0x424D;				// the start bytes turn up inside the data
		PM25<FakeStream> sensor; run(sensor, 0, 5);
		PM25_AQI_Data d = {};
		feed(makeFrame(s));
		check(run(sensor, 5, 5, &d) == 1 && d.particles_03um == 0x424D && d.particles_05um == 0x424D, "start bytes inside the data are just data");
	}

	std::printf("bad frames\n");
	{
		PM25<FakeStream> sensor; run(sensor, 0, 5);
		PM25_AQI_Data d = {};
		feed(good); run(sensor, 5, 5, &d);
		Sample other; other.pm25s = 999;
		std::vector<uint8_t> bad = makeFrame(other); bad[8] ^= 0x01;	// corrupted on the wire
		feed(bad);
		check(run(sensor, 10, 5) == 0, "a frame with a wrong checksum gives no reading");
		sensor.getData(&d);
		check(d.pm25_standard == 12, "and the last good values are left as they were");
	}
	{
		PM25<FakeStream> sensor; run(sensor, 0, 5);
		std::vector<uint8_t> wrongLen = good; wrongLen[3] = 20;	// length field not 28
		uint16_t sum = 0; for (int i = 0; i < 30; ++i) sum = (uint16_t)(sum + wrongLen[i]);
		wrongLen[30] = (uint8_t)(sum >> 8); wrongLen[31] = (uint8_t)(sum & 0xFF);
		feed(wrongLen);
		check(run(sensor, 5, 5) == 0, "a frame with the wrong length field is not accepted, even with a matching checksum");
	}

	std::printf("frames arriving faster than main() runs\n");
	{
		PM25<FakeStream> sensor; run(sensor, 0, 5);
		Sample a; a.pm25s = 100; a.p03 = 1111;
		Sample b; b.pm25s = 200; b.p03 = 2222;
		feed(makeFrame(a)); feed(makeFrame(b));					// both before main() gets a turn
		PM25_AQI_Data d = {};
		check(run(sensor, 5, 5, &d) == 1 && d.pm25_standard == 100 && d.particles_03um == 1111, "the first is delivered whole; the second is dropped, not mixed into it");
		feed(makeFrame(b));
		check(run(sensor, 10, 5, &d) == 1 && d.pm25_standard == 200, "the next one after that is delivered normally");
	}

	std::printf("sensor goes quiet\n");
	{
		int attaches = 0;
		PM25<FakeStream> sensor(&attaches);
		uint32_t t = 0;
		run(sensor, t, 5); t += 5; feed(good); run(sensor, t, 5); t += 5;
		check(sensor.state() == pm25_listening_state && attaches == 1, "listening, attached to the transport once");
		run(sensor, t, 4900); t += 4900;
		check(sensor.state() == pm25_listening_state, "4.9 s of silence is still tolerated");
		run(sensor, t, 200); t += 200;
		check(sensor.state() == pm25_error_state, "after 5 s it is reported through state()");
		const int before = attaches;
		run(sensor, t, 3100); t += 3100;
		check(attaches == before + 3, "once a second it asks the transport to start its receiver again");
		PM25_AQI_Data d = {};
		check(sensor.getData(&d) && d.pm25_standard == 12, "the last reading is still there to be read");
		feed(good);
		check(run(sensor, t, 5) == 1 && sensor.state() == pm25_listening_state, "a frame brings it straight back");
	}

	std::printf("tick counter rollover\n");
	{
		PM25<FakeStream> sensor;
		uint32_t t = 0xFFFFFFFFu - 2500;	// wraps 2.5 s into the run
		int n = 0; bool everError = false;
		for (int i = 0; i < 6; ++i) {
			for (uint32_t k = 0; k < 1000; ++k) { sensor.main(t + k); if (sensor.newData()) { PM25_AQI_Data d; sensor.getData(&d); ++n; } if (sensor.state() == pm25_error_state) everError = true; }
			t += 1000; feed(good);
		}
		check(n == 5 && !everError, "frames keep arriving across the wrap, and silence is not reported by mistake");
	}

	std::printf("xPM25: woken by the interrupt\n");
	{
		xPM25<FakeStream> sensor;
		g_rtos = SimRtos(); static int thread; g_rtos.current = &thread;
		sensor.main(0);											// attaches
		g_rtos.whileBlocked = [&] { feed(good); };				// a frame arrives while the thread sleeps
		sensor.main(1);
		check(g_rtos.blocks == 1 && g_rtos.lastTimeout == 4999, "with no frame, main() puts the thread to sleep, for as long as silence is allowed");
		check(g_rtos.sets == 1 && (g_rtos.flags[&thread] & 0x20000000u) == 0, "the frame's last byte wakes that thread, once");
		sensor.main(2);
		PM25_AQI_Data d = {};
		check(sensor.newData() && sensor.getData(&d) && d.pm25_standard == 12, "and the next main() has the reading");

		feed(good);												// a frame arrives while the thread is busy elsewhere
		g_rtos.blocks = 0; g_rtos.flags.clear();
		sensor.main(3); sensor.main(4);
		check(g_rtos.blocks == 0 || sensor.newData(), "a frame already waiting is never slept through");
	}

	std::printf("PM25<Stm32HalUartTransport>: the real UART transport over a simulated HAL\n");
	{
		UART_HandleTypeDef huart = {}; fakeUartInit(&huart);
		PM25<Stm32HalUartTransport> sensor(&huart);
		PM25_AQI_Data d = {};
		run(sensor, 0, 5);
		for (uint8_t b : good) fakeRxByte(&huart, b);
		check(run(sensor, 5, 5, &d) == 1 && d.pm25_standard == 12 && huart.bytesLost == 0, "a frame through HAL_UART_Receive_IT, one byte at a time");

		for (int i = 0; i < 10; ++i) fakeRxByte(&huart, good[i]);
		fakeOverrun(&huart);									// an interrupt was held off too long
		for (int i = 12; i < 32; ++i) fakeRxByte(&huart, good[i]);
		check(run(sensor, 10, 5) == 0, "an overrun spoils the frame it lands in");
		check(huart.bytesLost == 0 && huart.RxState == HAL_UART_STATE_BUSY_RX, "but every byte after it is still received: the receiver was started again");
		int n = 0;
		for (int f = 0; f < 2; ++f) { for (uint8_t b : good) fakeRxByte(&huart, b); n += run(sensor, 15 + 5 * f, 5); }
		check(n >= 1, "and readings resume within the next two frames");
	}
	{
		UART_HandleTypeDef huart = {};							// not initialised yet, as for an object built before main()
		PM25<Stm32HalUartTransport> sensor(&huart);
		run(sensor, 0, 100);
		fakeUartInit(&huart);									// MX_USARTx_UART_Init() runs after the constructor
		for (uint8_t b : good) fakeRxByte(&huart, b);
		check(run(sensor, 100, 5) == 0 && huart.bytesLost == 32, "built before the UART was set up: nothing is received at first");
		run(sensor, 105, 6000);									// silence, then the driver asks for the receiver again
		for (uint8_t b : good) fakeRxByte(&huart, b);
		check(run(sensor, 6105, 5) == 1, "it recovers by itself once the silence timeout has asked the transport to start receiving");
	}

	std::printf("\n%s\n", g_failures ? "FAILED" : "all passed");
	return g_failures ? 1 : 0;
}
