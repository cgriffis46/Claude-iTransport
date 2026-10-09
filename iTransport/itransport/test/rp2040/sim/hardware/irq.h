#pragma once
// Stand-in for the Pico SDK's hardware/irq.h. See pico_sim.h.
#include "../pico_sim.h"
#ifdef SIM_RP2350
enum { DMA_IRQ_0 = 10, DMA_IRQ_1 = 11, DMA_IRQ_2 = 12, DMA_IRQ_3 = 13, SPI0_IRQ = 31, SPI1_IRQ = 32,
       UART0_IRQ = 33, UART1_IRQ = 34, I2C0_IRQ = 36, I2C1_IRQ = 37 };
#else
enum { DMA_IRQ_0 = 11, DMA_IRQ_1 = 12, SPI0_IRQ = 18, SPI1_IRQ = 19, UART0_IRQ = 20, UART1_IRQ = 21,
       I2C0_IRQ = 23, I2C1_IRQ = 24 };
#endif
#define PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY 0x80
typedef void (*irq_handler_t)(void);
void irq_add_shared_handler(uint num, irq_handler_t handler, uint8_t order_priority);
void irq_set_enabled(uint num, bool enabled);
namespace sim { extern std::vector<irq_handler_t> handlers[SIM_NUM_IRQS]; extern bool irqEnabled[SIM_NUM_IRQS]; }
