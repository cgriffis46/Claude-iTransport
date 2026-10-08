/*
 * xhd44780.h
 *
 *  HD44780 for FreeRTOS (CMSIS-RTOS2). The state machine is hd44780's,
 *  unchanged. Only sleep() differs: while a transfer is in flight, and
 *  through the power-up and reset waits, it gives up the CPU instead of
 *  returning at once, so the thread that flushes (the GUI task,
 *  iDisplay/hw/freertos/xGui.h) does not spin.
 *
 *      xhd44780<Stm32HalI2CTransport, 20, 4> lcd(hd44780_pcf8574_param(), &hi2c1, HD44780_PCF8574_ADDR, i2c1Mutex);
 *
 *  sleep() is asked for ms and osDelay() takes ticks. They are the
 *  same at the usual 1 kHz tick. Convert here if yours differs.
 */

#ifndef XHD44780_H_
#define XHD44780_H_

#include "cmsis_os2.h"
#include "hd44780.h"

namespace HD44780 {

template <typename TTransport, uint8_t COLS = 16, uint8_t ROWS = 2>
class xhd44780 : public hd44780<TTransport, COLS, ROWS> {
public:
	using hd44780<TTransport, COLS, ROWS>::hd44780;

protected:
	void sleep(uint32_t ms) override { osDelay(ms); }
};

} /* namespace HD44780 */

#endif /* XHD44780_H_ */
