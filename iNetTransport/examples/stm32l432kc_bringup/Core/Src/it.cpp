/*
 * it.cpp — fault handlers and FreeRTOS hooks: each says what happened on
 * the log before stopping, which is most of what bring-up needs.
 */

#include <stdio.h>
#include "main.h"
#include "FreeRTOS.h"
#include "task.h"
#include "log.h"

extern "C" {

void NMI_Handler(void) { panic("NMI"); }
void MemManage_Handler(void) { panic("MemManage fault"); }
void BusFault_Handler(void) { panic("BusFault"); }
void UsageFault_Handler(void) { panic("UsageFault"); }
void DebugMon_Handler(void) {}

// Says where: the stacked PC and LR of the code that faulted.
void HardFault_Report(uint32_t *frame) {
	static char msg[96];
	snprintf(msg, sizeof msg, "HardFault at pc=0x%08lx lr=0x%08lx cfsr=0x%08lx",
	         static_cast<unsigned long>(frame[6]), static_cast<unsigned long>(frame[5]),
	         static_cast<unsigned long>(SCB->CFSR));
	panic(msg);
}

__attribute__((naked)) void HardFault_Handler(void) {
	__asm volatile(
		"tst lr, #4      \n"
		"ite eq          \n"
		"mrseq r0, msp   \n"
		"mrsne r0, psp   \n"
		"b HardFault_Report \n");
}

void vApplicationStackOverflowHook(TaskHandle_t, char *name) {
	static char msg[48];
	snprintf(msg, sizeof msg, "stack overflow in task '%s'", name);
	panic(msg);
}

void vApplicationMallocFailedHook(void) { panic("FreeRTOS heap exhausted: raise configTOTAL_HEAP_SIZE"); }

void vAssertCalled(const char *file, int line) {
	static char msg[96];
	snprintf(msg, sizeof msg, "assert failed: %s:%d", file, line);
	panic(msg);
}

} // extern "C"
