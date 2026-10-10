/*
 * xublox_gps.h
 *
 *  ublox_gps for FreeRTOS (CMSIS-RTOS2). The driver is ublox_gps's,
 *  unchanged. Only the two hooks differ: sleep() puts the thread to
 *  sleep, and wake(), called from the UART interrupt at the end of
 *  each NMEA line (and every 64 bytes), wakes it again.
 *
 *      xublox_gps<Stm32HalUartTransport> gps(param, &huart2);
 *
 *      for(;;){
 *          gps.main(osKernelGetTickCount());
 *      }
 *
 *  Give the receiver its own thread: main() must always be called from
 *  the same one, since that is the thread wake() wakes. The UART
 *  interrupt must be allowed to call FreeRTOS (its priority numerically
 *  at or above configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY).
 *
 *  sleep() is asked for ms and CMSIS-RTOS2 waits in ticks. They are the
 *  same at the usual 1 kHz tick. Convert here if yours differs.
 */

#ifndef XUBLOX_GPS_H_
#define XUBLOX_GPS_H_

#include "cmsis_os2.h"
#include "ublox_gps.h"

template <typename TTransport>
class xublox_gps : public ublox_gps<TTransport> {
public:
	using ublox_gps<TTransport>::ublox_gps;

protected:
	// The thread is noted first and the waiting bytes checked second,
	// as in xPM25: a wake() before the check is seen by it, one after
	// it sets the flag and ends the wait at once.
	void sleep(uint32_t ms) override {
		_thread = osThreadGetId();
		if (!this->wakePending()) {
			osThreadFlagsWait(xublox_wake_flag, osFlagsWaitAny, ms);
		}
	}

	// Interrupt context.
	void wake() override {
		if (_thread != nullptr) {
			osThreadFlagsSet(_thread, xublox_wake_flag);
		}
	}

private:
	// Not FreeRtosTransport's flag, nor xPM25's.
	static const uint32_t xublox_wake_flag = 0x10000000u;

	osThreadId_t _thread = nullptr;
};

#endif /* XUBLOX_GPS_H_ */
