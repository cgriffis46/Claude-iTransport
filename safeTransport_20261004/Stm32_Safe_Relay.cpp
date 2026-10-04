#include "Stm32_Safe_Relay.h"

Stm32_Safe_Relay::Stm32_Safe_Relay(UART_HandleTypeDef* huart1, UART_HandleTypeDef* huart2,
                                    const uint8_t* serialNumber, size_t serialNumberLen,
                                    uint32_t timeoutTicks)
    : transport1_(huart1),
      transport2_(huart2),
      loopback_(transport1_, transport2_, serialNumber, serialNumberLen, timeoutTicks) {}
