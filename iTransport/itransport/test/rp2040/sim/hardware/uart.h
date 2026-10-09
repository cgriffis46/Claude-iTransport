#pragma once
// Stand-in for the Pico SDK's hardware/uart.h. See pico_sim.h.
#include "../pico_sim.h"
#define UART_UARTFR_RXFE_BITS  0x00000010u
#define UART_UARTICR_OEIC_BITS 0x00000400u
struct uart_hw_t { SimReg dr, fr, icr; };
struct uart_inst_t;
extern uart_hw_t sim_uart_hw[2];
#define uart0 ((uart_inst_t*)&sim_uart_hw[0])
#define uart1 ((uart_inst_t*)&sim_uart_hw[1])
static inline uart_hw_t* uart_get_hw(uart_inst_t* u) { return (uart_hw_t*)u; }
static inline uint uart_get_index(uart_inst_t* u) { return (uart_hw_t*)u == &sim_uart_hw[1] ? 1u : 0u; }
static inline uint uart_get_dreq(uart_inst_t* u, bool is_tx) { return 20u + uart_get_index(u) * 2u + (is_tx ? 0u : 1u); }
static inline bool uart_is_enabled(uart_inst_t* u) { return sim::uart[uart_get_index(u)].enabled; }
static inline void uart_set_irqs_enabled(uart_inst_t* u, bool rx, bool tx) { (void)tx; sim::uart[uart_get_index(u)].rxIrq = rx; }
