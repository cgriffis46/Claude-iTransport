#pragma once
// Stand-in for the Pico SDK's hardware/gpio.h. See pico_sim.h.
#include "../pico_sim.h"
#define GPIO_OUT true
static inline void gpio_init(uint gpio) { sim::gpioOut[gpio] = false; }
static inline void gpio_set_dir(uint gpio, bool out) { sim::gpioOut[gpio] = out; }
static inline void gpio_put(uint gpio, bool value) { sim::gpioLevel[gpio] = value ? 1 : 0; }
