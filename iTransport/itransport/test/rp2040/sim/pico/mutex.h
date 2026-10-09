#pragma once
// Stand-in for the Pico SDK's pico/mutex.h: no second core, so a
// mutex is held or not. See pico_sim.h.
#include <cstdint>
struct mutex_t { bool held = false; };
static inline void mutex_init(mutex_t* m) { m->held = false; }
static inline bool mutex_try_enter(mutex_t* m, uint32_t* owner_out) { (void)owner_out; if (m->held) return false; m->held = true; return true; }
static inline void mutex_exit(mutex_t* m) { m->held = false; }
