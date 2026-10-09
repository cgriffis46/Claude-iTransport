#pragma once
// Stand-in for the Pico SDK's hardware/irq.h. See pico_sim.h.
#include "../pico_sim.h"
enum { DMA_IRQ_0 = 11, DMA_IRQ_1 = 12, SPI0_IRQ = 18, SPI1_IRQ = 19, UART0_IRQ = 20, UART1_IRQ = 21,
       I2C0_IRQ = 23, I2C1_IRQ = 24 };
#define PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY 0x80
typedef void (*irq_handler_t)(void);
void irq_add_shared_handler(uint num, irq_handler_t handler, uint8_t order_priority);
void irq_set_enabled(uint num, bool enabled);
namespace sim { extern std::vector<irq_handler_t> handlers[32]; extern bool irqEnabled[32]; }
