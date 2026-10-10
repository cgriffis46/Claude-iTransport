/*
 * xrfm95.h
 *
 *  rfm95 for FreeRTOS (CMSIS-RTOS2). The driver is rfm95's, unchanged.
 *  Only the two hooks differ: sleep() puts the thread to sleep, and
 *  wake(), called from onDio0()/onDio1() in the DIO pins' interrupts,
 *  wakes it again, so a TxDone or RxDone is handled at once rather than
 *  at the next poll.
 *
 *      xrfm95<Stm32HalSPITransport> radio(param, &hspi1, RFM_CS_GPIO_Port, RFM_CS_Pin, spi1Mutex);
 *      // in the EXTI callback: radio.onDio0(HAL_GetTick());
 *
 *      for(;;){ radio.main(osKernelGetTickCount()); }
 *
 *  Give the radio its own thread (main() always from the same one, the
 *  thread wake() wakes), and make requests from that thread too. The DIO
 *  interrupts must be allowed to call FreeRTOS (priority numerically at
 *  or above configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY). sleep() is in
 *  ms; at the usual 1 kHz tick that is the same as ticks.
 */

#ifndef XRFM95_H_
#define XRFM95_H_

#include "cmsis_os2.h"
#include "rfm95.h"

template <typename TTransport>
class xrfm95 : public rfm95<TTransport> {
public:
	using rfm95<TTransport>::rfm95;

protected:
	// The thread is noted first and the pending interrupt checked second:
	// an interrupt before the check is seen by it, one after it sets the
	// flag and ends the wait at once.
	void sleep(uint32_t ms) override {
		_thread = osThreadGetId();
		if (!this->irqPending()) {
			osThreadFlagsWait(xrfm95_wake_flag, osFlagsWaitAny, ms);
		}
	}

	// Interrupt context.
	void wake() override {
		if (_thread != nullptr) {
			osThreadFlagsSet(_thread, xrfm95_wake_flag);
		}
	}

private:
	// Not FreeRtosTransport's flag, nor xPM25's, xublox_gps's or xmtk3339's.
	static const uint32_t xrfm95_wake_flag = 0x04000000u;

	osThreadId_t _thread = nullptr;
};

#endif /* XRFM95_H_ */
