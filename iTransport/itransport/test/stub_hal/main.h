// A small simulation of the STM32 HAL UART calls, standing in for a
// CubeMX project's main.h, so Stm32HalUartTransport.cpp and
// Stm32UartItCallbacks.cpp — the real files, unmodified — can be
// compiled and run on a host.
//
// The part of the real handle the transports use is here under its
// real names. The fields below the line are the simulated peripheral.
// fake_hal.cpp implements the calls; the fake*() functions at the
// bottom are how a test plays the part of the hardware.
#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { HAL_OK = 0, HAL_ERROR, HAL_BUSY, HAL_TIMEOUT } HAL_StatusTypeDef;

typedef enum {
    HAL_UART_STATE_RESET   = 0x00U,
    HAL_UART_STATE_READY   = 0x20U,
    HAL_UART_STATE_BUSY_TX = 0x21U,
    HAL_UART_STATE_BUSY_RX = 0x22U
} HAL_UART_StateTypeDef;

#define HAL_UART_ERROR_NONE 0x00U
#define HAL_UART_ERROR_NE   0x02U
#define HAL_UART_ERROR_FE   0x04U
#define HAL_UART_ERROR_ORE  0x08U
#define UART_OVERSAMPLING_16 0x00U

typedef struct { volatile uint32_t CR1; volatile uint32_t BRR; } USART_TypeDef;
typedef struct { uint32_t BaudRate; uint32_t OverSampling; } UART_InitTypeDef;

typedef struct {
    USART_TypeDef*                 Instance;
    UART_InitTypeDef               Init;
    volatile HAL_UART_StateTypeDef gState;     // transmit side
    volatile HAL_UART_StateTypeDef RxState;    // receive side
    volatile uint32_t              ErrorCode;
    // ---- simulated peripheral, not in the real HAL ----
    uint8_t*       rxPtr;
    uint16_t       rxLeft;
    const uint8_t* txPtr;
    uint16_t       txLen;
    int            overrunPending;      // the overrun flag in the status register
    int            txCompletesAtOnce;   // transmit-complete interrupt fires inside HAL_UART_Transmit_IT
    int            bytesLost;           // bytes that arrived with no receive armed
    int            receivesArmed;
    int            receivesAborted;
} UART_HandleTypeDef;

#define __HAL_UART_DISABLE(h)       ((h)->Instance->CR1 &= ~1U)
#define __HAL_UART_ENABLE(h)        ((h)->Instance->CR1 |= 1U)
#define __HAL_UART_CLEAR_OREFLAG(h) ((h)->overrunPending = 0)

HAL_StatusTypeDef HAL_UART_Transmit_IT(UART_HandleTypeDef* huart, const uint8_t* pData, uint16_t Size);
HAL_StatusTypeDef HAL_UART_Receive_IT(UART_HandleTypeDef* huart, uint8_t* pData, uint16_t Size);
HAL_StatusTypeDef HAL_UART_Abort(UART_HandleTypeDef* huart);
HAL_StatusTypeDef HAL_UART_AbortReceive(UART_HandleTypeDef* huart);

// The application's callbacks — Stm32UartItCallbacks.cpp defines them.
void HAL_UART_TxCpltCallback(UART_HandleTypeDef* huart);
void HAL_UART_RxCpltCallback(UART_HandleTypeDef* huart);
void HAL_UART_ErrorCallback(UART_HandleTypeDef* huart);

// ---- the hardware's side, driven by the test ----
void fakeUartInit(UART_HandleTypeDef* huart);            // what MX_USARTx_UART_Init() leaves behind
void fakeRxByte(UART_HandleTypeDef* huart, uint8_t b);   // a character arrives on the line
void fakeOverrun(UART_HandleTypeDef* huart);             // a character arrived before the last was collected
void fakeNoise(UART_HandleTypeDef* huart);               // noise flag on a character; reception carries on
void fakeTxDone(UART_HandleTypeDef* huart);              // the last character has left the shift register

#ifdef __cplusplus
}
#endif
