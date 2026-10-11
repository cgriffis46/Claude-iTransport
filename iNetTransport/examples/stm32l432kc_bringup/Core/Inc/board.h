/*
 * board.h — clocks and peripherals of the NUCLEO-L432KC, set up by hand
 * (what CubeMX would generate), so this firmware builds without a
 * CubeMX project.
 */
#ifndef BRINGUP_BOARD_H
#define BRINGUP_BOARD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void board_clock_init(void);     /* 80 MHz from MSI + PLL, MSI trimmed by LSE when present */
void board_init(void);           /* GPIO, DMA, SPI1, USART1, USART2, interrupts */
int  board_lse_ok(void);         /* the LSE crystal started */
void board_spi_set_prescaler(uint32_t prescaler); /* SPI_BAUDRATEPRESCALER_x: re-initialises SPI1 */
uint32_t board_spi_hz(void);     /* SPI1's clock now */
const char *board_reset_cause(void);
void board_w5500_reset(void);    /* RSTn low 2 ms, then 60 ms for its PLL to lock (blocking, thread context) */
void board_esp_enable(int on);   /* EN high: running */
void board_reset_pin(int asserted); /* PA3 (the W5500's RSTn, the ATWINC1500's RESET_N) low while asserted */
uint32_t board_ms(void);         /* HAL_GetTick() */

#ifdef __cplusplus
}
#endif

#endif /* BRINGUP_BOARD_H */
