/*
 * main.h — pins and handles of the bring-up firmware. The transports
 * include "main.h" for the HAL, as in a CubeMX project.
 *
 * NUCLEO-L432KC (Arduino Nano pin names in brackets):
 *
 *   W5500 module          ESP-AT module (ESP-01, ESP32 DevKit, ...)
 *   SCK   PA5  [A4]       TX  -> PA10 [D0]  USART1_RX
 *   MISO  PA6  [A5]       RX  <- PA9  [D1]  USART1_TX
 *   MOSI  PA7  [A6]       EN     PA8  [D9]
 *   CS    PA4  [A3]
 *   INT   PA1  [A1]       Log: USART2 on the ST-LINK virtual COM port
 *   RSTn  PA3  [A2]            (PA2 TX, PA15 RX), 115200 8N1
 *                         LED: LD3, PB3 [D13]
 */
#ifndef MAIN_H
#define MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

#include "stm32l4xx_hal.h"

#define W5500_CS_Pin        GPIO_PIN_4
#define W5500_CS_GPIO_Port  GPIOA
#define W5500_INT_Pin       GPIO_PIN_1
#define W5500_INT_GPIO_Port GPIOA
#define W5500_RST_Pin       GPIO_PIN_3
#define W5500_RST_GPIO_Port GPIOA
#define ESP_EN_Pin          GPIO_PIN_8
#define ESP_EN_GPIO_Port    GPIOA
#define LED_Pin             GPIO_PIN_3
#define LED_GPIO_Port       GPIOB

extern SPI_HandleTypeDef  hspi1;
extern UART_HandleTypeDef huart1;   /* ESP-AT module */
extern UART_HandleTypeDef huart2;   /* log */

void Error_Handler(void);

#ifdef __cplusplus
}
#endif

#endif /* MAIN_H */
