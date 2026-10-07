// The simulated HAL UART behind stub_hal/main.h. It follows the real
// HAL in the respects the transports depend on:
//   - HAL_UART_Receive_IT refuses (HAL_BUSY) unless the receive side
//     is READY, and after `Size` characters calls
//     HAL_UART_RxCpltCallback with the receive side READY again.
//   - An overrun ends the receive (READY) and calls
//     HAL_UART_ErrorCallback with HAL_UART_ERROR_ORE in ErrorCode.
//     Arming a receive with the overrun flag still set does the same
//     at once.
//   - A noise flag calls HAL_UART_ErrorCallback but the receive stays
//     armed.
//   - HAL_UART_Transmit_IT refuses (HAL_BUSY) while a send is in
//     flight, and HAL_UART_TxCpltCallback is called with the transmit
//     side READY again.
#include "main.h"

extern "C" {

void fakeUartInit(UART_HandleTypeDef* h) {
    static USART_TypeDef regs;
    h->Instance = &regs;
    h->Init.BaudRate = 9600;
    h->Init.OverSampling = UART_OVERSAMPLING_16;
    h->gState = HAL_UART_STATE_READY;
    h->RxState = HAL_UART_STATE_READY;
    h->ErrorCode = HAL_UART_ERROR_NONE;
    h->rxPtr = nullptr; h->rxLeft = 0; h->txPtr = nullptr; h->txLen = 0;
    h->overrunPending = 0;
}

HAL_StatusTypeDef HAL_UART_Receive_IT(UART_HandleTypeDef* h, uint8_t* pData, uint16_t Size) {
    if (h->RxState != HAL_UART_STATE_READY) return HAL_BUSY;
    if (pData == nullptr || Size == 0) return HAL_ERROR;
    h->rxPtr = pData; h->rxLeft = Size;
    h->RxState = HAL_UART_STATE_BUSY_RX;
    ++h->receivesArmed;
    if (h->overrunPending) fakeOverrun(h);   // the interrupt fires as soon as it is enabled
    return HAL_OK;
}

HAL_StatusTypeDef HAL_UART_Transmit_IT(UART_HandleTypeDef* h, const uint8_t* pData, uint16_t Size) {
    if (h->gState != HAL_UART_STATE_READY) return HAL_BUSY;
    if (pData == nullptr || Size == 0) return HAL_ERROR;
    h->txPtr = pData; h->txLen = Size;
    h->gState = HAL_UART_STATE_BUSY_TX;
    if (h->txCompletesAtOnce) fakeTxDone(h); // the interrupt beats the return to the caller
    return HAL_OK;
}

HAL_StatusTypeDef HAL_UART_AbortReceive(UART_HandleTypeDef* h) {
    if (h->RxState == HAL_UART_STATE_BUSY_RX) ++h->receivesAborted;
    h->RxState = HAL_UART_STATE_READY;
    h->rxPtr = nullptr; h->rxLeft = 0;
    return HAL_OK;
}

HAL_StatusTypeDef HAL_UART_Abort(UART_HandleTypeDef* h) {
    HAL_UART_AbortReceive(h);
    h->gState = HAL_UART_STATE_READY;
    return HAL_OK;
}

void fakeRxByte(UART_HandleTypeDef* h, uint8_t b) {
    if (h->RxState != HAL_UART_STATE_BUSY_RX) { ++h->bytesLost; return; }
    *h->rxPtr++ = b;
    if (--h->rxLeft == 0) {
        h->RxState = HAL_UART_STATE_READY;
        HAL_UART_RxCpltCallback(h);
    }
}

void fakeOverrun(UART_HandleTypeDef* h) {
    h->overrunPending = 0;                   // HAL clears the flag as it handles it
    h->RxState = HAL_UART_STATE_READY;       // and gives up on the receive
    h->ErrorCode = HAL_UART_ERROR_ORE;
    HAL_UART_ErrorCallback(h);
    h->ErrorCode = HAL_UART_ERROR_NONE;
}

void fakeNoise(UART_HandleTypeDef* h) {
    h->ErrorCode = HAL_UART_ERROR_NE;
    HAL_UART_ErrorCallback(h);
    h->ErrorCode = HAL_UART_ERROR_NONE;
}

void fakeTxDone(UART_HandleTypeDef* h) {
    if (h->gState != HAL_UART_STATE_BUSY_TX) return;
    h->gState = HAL_UART_STATE_READY;
    HAL_UART_TxCpltCallback(h);
}

} // extern "C"
