/*
 * xLoRaWanMac.h
 *
 *  The LoRaWAN MAC's thread loop for CMSIS-RTOS2 (FreeRTOS). The Mac and
 *  its radio are the plain ones; this sleeps between steps for as long
 *  as Mac::sleepHintMs() says (never past a receive window or a
 *  retransmission), and the radio's DIO interrupts wake it at once.
 *
 *      static rfm95<Stm32HalSPITransport> radio(rp, &hspi1, RFM_CS_GPIO_Port, RFM_CS_Pin, spi1Mutex);
 *      static lorawan::RegionUS915 region(2);
 *      static FlashStore store;                       // your iSessionStore
 *      static lorawan::Mac mac(radio, region, store, param);
 *      static lorawan::xLoRaWanMac loop(mac);
 *
 *      // EXTI on DIO0 (and DIO1):
 *      //   radio.onDio0(HAL_GetTick()); loop.wakeFromIsr();
 *
 *      void lorawanTask(void*) {
 *          mac.begin(osKernelGetTickCount());
 *          if (!mac.joined()) mac.join();
 *          for (;;) {
 *              loop.step(osKernelGetTickCount());
 *              lorawan::MacEvent e; uint8_t buf[242];
 *              while (mac.takeEvent(&e, buf, sizeof buf)) ...
 *              // requests (send(), join()) from this thread too
 *          }
 *      }
 *
 *  Everything that touches the Mac or the radio runs in this one thread;
 *  only onDio0()/onDio1() and wakeFromIsr() come from the interrupt (whose
 *  priority must allow FreeRTOS calls). The sleep is in kernel ticks,
 *  which are ms at the usual 1 kHz tick. A flag set while the thread is
 *  running stays set, so a wake between main() and the wait is not lost.
 */

#ifndef XLORAWANMAC_H_
#define XLORAWANMAC_H_

#include "cmsis_os2.h"
#include "LoRaWanMac.h"

namespace lorawan {

class xLoRaWanMac {
public:
	// Not xrfm95's flag (0x04000000), nor FreeRtosTransport's, xPM25's or the GNSS drivers'.
	static const uint32_t kWakeFlag = 0x08000000u;

	explicit xLoRaWanMac(Mac& mac) : _mac(mac) {}

	// One step: the MAC (and the radio), then sleep until the next thing
	// to do or a wake.
	void step(uint32_t nowMs) {
		_thread = osThreadGetId();
		_mac.main(nowMs);
		const uint32_t ms = _mac.sleepHintMs(nowMs);
		if (ms) osThreadFlagsWait(kWakeFlag, osFlagsWaitAny, ms);
	}

	// From the DIO interrupts, after the radio's onDio0()/onDio1().
	void wakeFromIsr() {
		osThreadId_t t = _thread;
		if (t != nullptr) osThreadFlagsSet(t, kWakeFlag);
	}

private:
	Mac& _mac;
	osThreadId_t volatile _thread = nullptr;
};

} // namespace lorawan

#endif /* XLORAWANMAC_H_ */
