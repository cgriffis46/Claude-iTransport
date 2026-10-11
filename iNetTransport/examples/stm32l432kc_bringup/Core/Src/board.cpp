/*
 * board.cpp — clocks, pins and peripherals of the NUCLEO-L432KC for the
 * bring-up firmware: what CubeMX would generate, written out so the
 * firmware needs no CubeMX project. See main.h for the pin map.
 */

#include "main.h"
#include "board.h"
#include "config.h"

SPI_HandleTypeDef  hspi1;
UART_HandleTypeDef huart1;
UART_HandleTypeDef huart2;
static DMA_HandleTypeDef hdma_spi1_rx;
static DMA_HandleTypeDef hdma_spi1_tx;

static int s_lseOk = 0;
static uint32_t s_resetFlags = 0;

// Interrupts that call FreeRTOS: numerically at or above
// configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY (5).
static const uint32_t kIrqPriority = 6;

// A HAL call failed during start-up, before the log is up.
extern "C" void Error_Handler(void) {
	__disable_irq();
	for (;;) {
		HAL_GPIO_TogglePin(LED_GPIO_Port, LED_Pin);
		for (volatile uint32_t i = 0; i < 400000; ++i) {}
	}
}

// ---- HAL time base on TIM6, leaving SysTick to FreeRTOS ----

extern "C" HAL_StatusTypeDef HAL_InitTick(uint32_t TickPriority) {
	__HAL_RCC_TIM6_CLK_ENABLE();
	// TIM6 is on APB1; with the APB1 prescaler at 1 it counts at PCLK1.
	const uint32_t clk = HAL_RCC_GetPCLK1Freq();
	TIM6->CR1 = 0;
	TIM6->PSC = clk / 1000000U - 1U;   // 1 MHz
	TIM6->ARR = 1000U - 1U;            // 1 kHz
	TIM6->EGR = TIM_EGR_UG;
	TIM6->SR = 0;
	TIM6->DIER = TIM_DIER_UIE;
	TIM6->CR1 = TIM_CR1_CEN;
	HAL_NVIC_SetPriority(TIM6_DAC_IRQn, TickPriority, 0);
	HAL_NVIC_EnableIRQ(TIM6_DAC_IRQn);
	uwTickPrio = TickPriority;
	return HAL_OK;
}

extern "C" void HAL_SuspendTick(void) { TIM6->DIER &= ~TIM_DIER_UIE; }
extern "C" void HAL_ResumeTick(void) { TIM6->DIER |= TIM_DIER_UIE; }

extern "C" void TIM6_DAC_IRQHandler(void) {
	if (TIM6->SR & TIM_SR_UIF) {
		TIM6->SR = 0;
		HAL_IncTick();
	}
}

// ---- clocks ----

void board_clock_init(void) {
	s_resetFlags = RCC->CSR;
	__HAL_RCC_CLEAR_RESET_FLAGS();

	__HAL_RCC_PWR_CLK_ENABLE();
	if (HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1) != HAL_OK) Error_Handler();

	// LSE (X2, 32.768 kHz) trims the MSI to crystal accuracy, which the
	// UARTs appreciate. If it doesn't start, the factory-trimmed MSI
	// (about 1 %) still does.
	HAL_PWR_EnableBkUpAccess();
	__HAL_RCC_LSEDRIVE_CONFIG(RCC_LSEDRIVE_LOW);
	RCC_OscInitTypeDef lse = {};
	lse.OscillatorType = RCC_OSCILLATORTYPE_LSE;
	lse.LSEState = RCC_LSE_ON;
	lse.PLL.PLLState = RCC_PLL_NONE;
	s_lseOk = HAL_RCC_OscConfig(&lse) == HAL_OK;

	// MSI 4 MHz -> PLL x40 /2 -> 80 MHz.
	RCC_OscInitTypeDef osc = {};
	osc.OscillatorType = RCC_OSCILLATORTYPE_MSI;
	osc.MSIState = RCC_MSI_ON;
	osc.MSICalibrationValue = RCC_MSICALIBRATION_DEFAULT;
	osc.MSIClockRange = RCC_MSIRANGE_6;
	osc.PLL.PLLState = RCC_PLL_ON;
	osc.PLL.PLLSource = RCC_PLLSOURCE_MSI;
	osc.PLL.PLLM = 1;
	osc.PLL.PLLN = 40;
	osc.PLL.PLLP = RCC_PLLP_DIV7;
	osc.PLL.PLLQ = RCC_PLLQ_DIV2;
	osc.PLL.PLLR = RCC_PLLR_DIV2;
	if (HAL_RCC_OscConfig(&osc) != HAL_OK) Error_Handler();

	RCC_ClkInitTypeDef clk = {};
	clk.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
	clk.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
	clk.AHBCLKDivider = RCC_SYSCLK_DIV1;
	clk.APB1CLKDivider = RCC_HCLK_DIV1;
	clk.APB2CLKDivider = RCC_HCLK_DIV1;
	if (HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_4) != HAL_OK) Error_Handler();

	if (s_lseOk) HAL_RCCEx_EnableMSIPLLMode();
}

int board_lse_ok(void) { return s_lseOk; }

const char *board_reset_cause(void) {
	if (s_resetFlags & RCC_CSR_FWRSTF) return "firewall";
	if (s_resetFlags & RCC_CSR_OBLRSTF) return "option bytes";
	if (s_resetFlags & RCC_CSR_LPWRRSTF) return "low power";
	if (s_resetFlags & RCC_CSR_WWDGRSTF) return "window watchdog";
	if (s_resetFlags & RCC_CSR_IWDGRSTF) return "watchdog";
	if (s_resetFlags & RCC_CSR_SFTRSTF) return "software";
	if (s_resetFlags & RCC_CSR_BORRSTF) return "power on / brown-out";
	if (s_resetFlags & RCC_CSR_PINRSTF) return "reset pin";
	return "unknown";
}

// ---- peripherals ----

static void gpio_init(void) {
	__HAL_RCC_GPIOA_CLK_ENABLE();
	__HAL_RCC_GPIOB_CLK_ENABLE();

	// Outputs, in their idle state first: CS high (deselected), W5500
	// out of reset, ESP held in reset until the driver releases it, LED off.
	HAL_GPIO_WritePin(W5500_CS_GPIO_Port, W5500_CS_Pin, GPIO_PIN_SET);
	HAL_GPIO_WritePin(W5500_RST_GPIO_Port, W5500_RST_Pin, GPIO_PIN_SET);
	HAL_GPIO_WritePin(ESP_EN_GPIO_Port, ESP_EN_Pin, GPIO_PIN_RESET);
	HAL_GPIO_WritePin(LED_GPIO_Port, LED_Pin, GPIO_PIN_RESET);

	GPIO_InitTypeDef g = {};
	g.Mode = GPIO_MODE_OUTPUT_PP;
	g.Pull = GPIO_NOPULL;
	g.Speed = GPIO_SPEED_FREQ_HIGH;
	g.Pin = W5500_CS_Pin | W5500_RST_Pin | ESP_EN_Pin;
	HAL_GPIO_Init(GPIOA, &g);
	g.Speed = GPIO_SPEED_FREQ_LOW;
	g.Pin = LED_Pin;
	HAL_GPIO_Init(LED_GPIO_Port, &g);

	// W5500 INT: open-drain, active low.
	g.Mode = GPIO_MODE_IT_FALLING;
	g.Pull = GPIO_PULLUP;
	g.Pin = W5500_INT_Pin;
	HAL_GPIO_Init(W5500_INT_GPIO_Port, &g);
	HAL_NVIC_SetPriority(EXTI1_IRQn, kIrqPriority, 0);
	HAL_NVIC_EnableIRQ(EXTI1_IRQn);
}

static void dma_init(void) {
	__HAL_RCC_DMA1_CLK_ENABLE();
	HAL_NVIC_SetPriority(DMA1_Channel2_IRQn, kIrqPriority, 0);
	HAL_NVIC_EnableIRQ(DMA1_Channel2_IRQn);
	HAL_NVIC_SetPriority(DMA1_Channel3_IRQn, kIrqPriority, 0);
	HAL_NVIC_EnableIRQ(DMA1_Channel3_IRQn);
}

static void spi1_init(uint32_t prescaler) {
	hspi1.Instance = SPI1;
	hspi1.Init.Mode = SPI_MODE_MASTER;
	hspi1.Init.Direction = SPI_DIRECTION_2LINES;
	hspi1.Init.DataSize = SPI_DATASIZE_8BIT;
	hspi1.Init.CLKPolarity = SPI_POLARITY_LOW;    // W5500: mode 0
	hspi1.Init.CLKPhase = SPI_PHASE_1EDGE;
	hspi1.Init.NSS = SPI_NSS_SOFT;
	hspi1.Init.BaudRatePrescaler = prescaler;
	hspi1.Init.FirstBit = SPI_FIRSTBIT_MSB;
	hspi1.Init.TIMode = SPI_TIMODE_DISABLE;
	hspi1.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
	hspi1.Init.CRCPolynomial = 7;
	hspi1.Init.CRCLength = SPI_CRC_LENGTH_DATASIZE;
	hspi1.Init.NSSPMode = SPI_NSS_PULSE_DISABLE;
	if (HAL_SPI_Init(&hspi1) != HAL_OK) Error_Handler();
}

void board_spi_set_prescaler(uint32_t prescaler) {
	HAL_SPI_DeInit(&hspi1);
	spi1_init(prescaler);
}

uint32_t board_spi_hz(void) {
	const uint32_t shift = (hspi1.Init.BaudRatePrescaler >> SPI_CR1_BR_Pos) + 1U; // /2 .. /256
	return HAL_RCC_GetPCLK2Freq() >> shift;
}

static void uart_init(UART_HandleTypeDef *h, USART_TypeDef *inst, uint32_t baud) {
	h->Instance = inst;
	h->Init.BaudRate = baud;
	h->Init.WordLength = UART_WORDLENGTH_8B;
	h->Init.StopBits = UART_STOPBITS_1;
	h->Init.Parity = UART_PARITY_NONE;
	h->Init.Mode = UART_MODE_TX_RX;
	h->Init.HwFlowCtl = UART_HWCONTROL_NONE;
	h->Init.OverSampling = UART_OVERSAMPLING_16;
	h->Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
	h->AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
	if (HAL_UART_Init(h) != HAL_OK) Error_Handler();
}

void board_init(void) {
	gpio_init();
	dma_init();
	spi1_init(SPI_BAUDRATEPRESCALER_256);
	uart_init(&huart2, USART2, 115200);
	uart_init(&huart1, USART1, ESP_BAUD);
}

void board_w5500_reset(void) {
	HAL_GPIO_WritePin(W5500_RST_GPIO_Port, W5500_RST_Pin, GPIO_PIN_RESET);
	HAL_Delay(2);
	HAL_GPIO_WritePin(W5500_RST_GPIO_Port, W5500_RST_Pin, GPIO_PIN_SET);
	HAL_Delay(60);
}

void board_reset_pin(int asserted) {
	HAL_GPIO_WritePin(W5500_RST_GPIO_Port, W5500_RST_Pin, asserted ? GPIO_PIN_RESET : GPIO_PIN_SET);
}

void board_esp_enable(int on) {
	HAL_GPIO_WritePin(ESP_EN_GPIO_Port, ESP_EN_Pin, on ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

uint32_t board_ms(void) { return HAL_GetTick(); }

// ---- MSP: pins, clocks, DMA and NVIC behind each HAL_xxx_Init ----

extern "C" void HAL_MspInit(void) {
	__HAL_RCC_SYSCFG_CLK_ENABLE();
	__HAL_RCC_PWR_CLK_ENABLE();
	HAL_NVIC_SetPriority(PendSV_IRQn, 15, 0);
}

extern "C" void HAL_SPI_MspInit(SPI_HandleTypeDef *h) {
	if (h->Instance != SPI1) return;
	__HAL_RCC_SPI1_CLK_ENABLE();
	__HAL_RCC_GPIOA_CLK_ENABLE();
	GPIO_InitTypeDef g = {};
	g.Pin = GPIO_PIN_5 | GPIO_PIN_6 | GPIO_PIN_7;    // SCK, MISO, MOSI
	g.Mode = GPIO_MODE_AF_PP;
	g.Pull = GPIO_NOPULL;
	g.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
	g.Alternate = GPIO_AF5_SPI1;
	HAL_GPIO_Init(GPIOA, &g);

	hdma_spi1_rx.Instance = DMA1_Channel2;
	hdma_spi1_rx.Init.Request = DMA_REQUEST_1;
	hdma_spi1_rx.Init.Direction = DMA_PERIPH_TO_MEMORY;
	hdma_spi1_rx.Init.PeriphInc = DMA_PINC_DISABLE;
	hdma_spi1_rx.Init.MemInc = DMA_MINC_ENABLE;
	hdma_spi1_rx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
	hdma_spi1_rx.Init.MemDataAlignment = DMA_MDATAALIGN_BYTE;
	hdma_spi1_rx.Init.Mode = DMA_NORMAL;
	hdma_spi1_rx.Init.Priority = DMA_PRIORITY_HIGH;
	if (HAL_DMA_Init(&hdma_spi1_rx) != HAL_OK) Error_Handler();
	__HAL_LINKDMA(h, hdmarx, hdma_spi1_rx);

	hdma_spi1_tx.Instance = DMA1_Channel3;
	hdma_spi1_tx.Init = hdma_spi1_rx.Init;
	hdma_spi1_tx.Init.Direction = DMA_MEMORY_TO_PERIPH;
	hdma_spi1_tx.Init.Priority = DMA_PRIORITY_MEDIUM;
	if (HAL_DMA_Init(&hdma_spi1_tx) != HAL_OK) Error_Handler();
	__HAL_LINKDMA(h, hdmatx, hdma_spi1_tx);

	HAL_NVIC_SetPriority(SPI1_IRQn, kIrqPriority, 0);
	HAL_NVIC_EnableIRQ(SPI1_IRQn);
}

extern "C" void HAL_SPI_MspDeInit(SPI_HandleTypeDef *h) {
	if (h->Instance != SPI1) return;
	__HAL_RCC_SPI1_CLK_DISABLE();
	HAL_GPIO_DeInit(GPIOA, GPIO_PIN_5 | GPIO_PIN_6 | GPIO_PIN_7);
	HAL_DMA_DeInit(h->hdmarx);
	HAL_DMA_DeInit(h->hdmatx);
	HAL_NVIC_DisableIRQ(SPI1_IRQn);
}

extern "C" void HAL_UART_MspInit(UART_HandleTypeDef *h) {
	GPIO_InitTypeDef g = {};
	g.Mode = GPIO_MODE_AF_PP;
	g.Pull = GPIO_PULLUP;
	g.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
	if (h->Instance == USART2) {       // ST-LINK virtual COM port
		__HAL_RCC_USART2_CLK_ENABLE();
		g.Pin = GPIO_PIN_2;
		g.Alternate = GPIO_AF7_USART2;
		HAL_GPIO_Init(GPIOA, &g);
		g.Pin = GPIO_PIN_15;
		g.Alternate = GPIO_AF3_USART2;
		HAL_GPIO_Init(GPIOA, &g);
	} else if (h->Instance == USART1) { // ESP-AT module
		__HAL_RCC_USART1_CLK_ENABLE();
		g.Pin = GPIO_PIN_9 | GPIO_PIN_10;
		g.Alternate = GPIO_AF7_USART1;
		HAL_GPIO_Init(GPIOA, &g);
		HAL_NVIC_SetPriority(USART1_IRQn, kIrqPriority, 0);
		HAL_NVIC_EnableIRQ(USART1_IRQn);
	}
}

// ---- interrupt handlers ----

extern "C" void SPI1_IRQHandler(void) { HAL_SPI_IRQHandler(&hspi1); }
extern "C" void DMA1_Channel2_IRQHandler(void) { HAL_DMA_IRQHandler(&hdma_spi1_rx); }
extern "C" void DMA1_Channel3_IRQHandler(void) { HAL_DMA_IRQHandler(&hdma_spi1_tx); }
extern "C" void USART1_IRQHandler(void) { HAL_UART_IRQHandler(&huart1); }
extern "C" void EXTI1_IRQHandler(void) { HAL_GPIO_EXTI_IRQHandler(W5500_INT_Pin); }
