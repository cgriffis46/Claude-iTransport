/*
 * log.h — timestamped lines on the ST-LINK virtual COM port (USART2,
 * 115200 8N1). Thread-safe once the kernel runs; never from an ISR.
 */
#ifndef BRINGUP_LOG_H
#define BRINGUP_LOG_H

#ifdef __cplusplus
extern "C" {
#endif

void log_init(void);
void log_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Fatal: prints straight to the UART registers (works with interrupts
 * off, from a fault handler), then blinks LD3 fast for ever. */
void panic(const char *msg) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif

#endif /* BRINGUP_LOG_H */
