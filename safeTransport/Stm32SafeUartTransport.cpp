#include "Stm32SafeUartTransport.h"

Stm32SafeUartTransport::Stm32SafeUartTransport(UART_HandleTypeDef* huart1, UART_HandleTypeDef* huart2,
                                                UART_HandleTypeDef* huartData,
                                                const uint8_t* serialNumber, size_t serialNumberLen,
                                                uint32_t timeoutTicks)
    : loopbackTransport1_(huart1),
      loopbackTransport2_(huart2),
      dataTransport_(huartData),
      loopback_(loopbackTransport1_, loopbackTransport2_, serialNumber, serialNumberLen, timeoutTicks) {}
