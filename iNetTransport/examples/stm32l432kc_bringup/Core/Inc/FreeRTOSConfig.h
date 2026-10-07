/*
 * FreeRTOSConfig.h — FreeRTOS on the STM32L432 (Cortex-M4F, 80 MHz) for
 * the bring-up firmware. CMSIS-RTOS2 on top (cmsis_os2.c), as CubeMX
 * sets it up.
 */
#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

#if defined(__GNUC__) && !defined(__ASSEMBLER__)
#include <stdint.h>
extern uint32_t SystemCoreClock;
#ifdef __cplusplus
extern "C"
#endif
void vAssertCalled(const char *file, int line);
#endif

#define CMSIS_device_header "stm32l4xx.h"

#define configENABLE_FPU                         1
#define configENABLE_MPU                         0
#define configUSE_PREEMPTION                     1
#define configSUPPORT_STATIC_ALLOCATION          0
#define configSUPPORT_DYNAMIC_ALLOCATION         1
#define configUSE_IDLE_HOOK                      0
#define configUSE_TICK_HOOK                      0
#define configCPU_CLOCK_HZ                       (SystemCoreClock)
#define configTICK_RATE_HZ                       ((TickType_t)1000)
#define configMAX_PRIORITIES                     (56)
#define configMINIMAL_STACK_SIZE                 ((uint16_t)128)
#define configTOTAL_HEAP_SIZE                    ((size_t)(34 * 1024))
#define configMAX_TASK_NAME_LEN                  (16)
#define configUSE_TRACE_FACILITY                 1
#define configUSE_16_BIT_TICKS                   0
#define configUSE_MUTEXES                        1
#define configQUEUE_REGISTRY_SIZE                8
#define configUSE_RECURSIVE_MUTEXES              1
#define configUSE_COUNTING_SEMAPHORES            1
#define configUSE_TASK_NOTIFICATIONS             1
#define configUSE_PORT_OPTIMISED_TASK_SELECTION  0
#define configCHECK_FOR_STACK_OVERFLOW           2
#define configUSE_MALLOC_FAILED_HOOK             1
#define configUSE_TIMERS                         1
#define configTIMER_TASK_PRIORITY                (2)
#define configTIMER_QUEUE_LENGTH                 10
#define configTIMER_TASK_STACK_DEPTH             256
#define configUSE_CO_ROUTINES                    0
#define configMESSAGE_BUFFER_LENGTH_TYPE         size_t

#define INCLUDE_vTaskPrioritySet             1
#define INCLUDE_uxTaskPriorityGet            1
#define INCLUDE_vTaskDelete                  1
#define INCLUDE_vTaskSuspend                 1
/* INCLUDE_xTaskDelayUntil: newer kernels derive it from the line below */
#define INCLUDE_vTaskDelayUntil              1
#define INCLUDE_vTaskDelay                   1
#define INCLUDE_xTaskGetSchedulerState       1
#define INCLUDE_xTimerPendFunctionCall       1
#define INCLUDE_xQueueGetMutexHolder         1
#define INCLUDE_xSemaphoreGetMutexHolder     1
#define INCLUDE_uxTaskGetStackHighWaterMark  1
#define INCLUDE_xTaskGetCurrentTaskHandle    1
#define INCLUDE_eTaskGetState                1
#define INCLUDE_xTaskAbortDelay              1

#ifdef __NVIC_PRIO_BITS
#define configPRIO_BITS __NVIC_PRIO_BITS
#else
#define configPRIO_BITS 4
#endif
#define configLIBRARY_LOWEST_INTERRUPT_PRIORITY      15
/* Interrupts that call FreeRTOS (SPI1, its DMA, EXTI1, USART1) must be
 * at this priority number or higher: board.cpp puts them at 6. */
#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY 5
#define configKERNEL_INTERRUPT_PRIORITY      (configLIBRARY_LOWEST_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))
#define configMAX_SYSCALL_INTERRUPT_PRIORITY (configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))

#define configASSERT(x) if ((x) == 0) vAssertCalled(__FILE__, __LINE__)

#define vPortSVCHandler    SVC_Handler
#define xPortPendSVHandler PendSV_Handler
/* SysTick belongs to FreeRTOS (cmsis_os2.c defines the handler); the
 * HAL's tick runs on TIM6 instead. */
#define USE_CUSTOM_SYSTICK_HANDLER_IMPLEMENTATION 0

#endif /* FREERTOS_CONFIG_H */
