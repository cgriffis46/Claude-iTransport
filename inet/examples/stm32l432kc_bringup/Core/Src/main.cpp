/*
 * main.cpp — bring-up firmware for the inet drivers on a NUCLEO-L432KC.
 * See BRINGUP.md for wiring, building, flashing and reading the log.
 */

#include "main.h"
#include "cmsis_os2.h"
#include "board.h"
#include "log.h"

void bringup_start();

int main(void) {
	HAL_Init();
	board_clock_init();
	board_init();
	log_init();

	osKernelInitialize();
	bringup_start();
	osKernelStart();

	panic("scheduler returned");
}
