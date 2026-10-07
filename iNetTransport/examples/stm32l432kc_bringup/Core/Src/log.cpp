/*
 * log.cpp — timestamped lines on USART2 (ST-LINK virtual COM port).
 */

#include <stdarg.h>
#include <stdio.h>
#include "main.h"
#include "cmsis_os2.h"
#include "log.h"

static osMutexId_t s_mutex = nullptr;
static char s_buf[256];

void log_init(void) {}

static bool kernelRunning() { return osKernelGetState() == osKernelRunning; }

void log_printf(const char *fmt, ...) {
	if (kernelRunning()) {
		if (s_mutex == nullptr) s_mutex = osMutexNew(nullptr); // first call is from the bring-up thread
		osMutexAcquire(s_mutex, osWaitForever);
	}
	const uint32_t t = HAL_GetTick();
	int n = snprintf(s_buf, sizeof s_buf, "[%5lu.%03lu] ", static_cast<unsigned long>(t / 1000),
	                 static_cast<unsigned long>(t % 1000));
	va_list ap;
	va_start(ap, fmt);
	const int m = vsnprintf(s_buf + n, sizeof s_buf - n - 2, fmt, ap);
	va_end(ap);
	if (m > 0) n += (m < static_cast<int>(sizeof s_buf) - n - 2) ? m : static_cast<int>(sizeof s_buf) - n - 3;
	s_buf[n++] = '\r';
	s_buf[n++] = '\n';
	HAL_UART_Transmit(&huart2, reinterpret_cast<uint8_t *>(s_buf), static_cast<uint16_t>(n), 200);
	if (kernelRunning()) osMutexRelease(s_mutex);
}

static void rawPut(const char *s) {
	for (; *s; ++s) {
		while ((USART2->ISR & USART_ISR_TXE) == 0) {}
		USART2->TDR = static_cast<uint8_t>(*s);
	}
}

void panic(const char *msg) {
	__disable_irq();
	rawPut("\r\n*** PANIC: ");
	rawPut(msg);
	rawPut("\r\n");
	for (;;) {
		HAL_GPIO_TogglePin(LED_GPIO_Port, LED_Pin);
		for (volatile uint32_t i = 0; i < 300000; ++i) {}
	}
}
