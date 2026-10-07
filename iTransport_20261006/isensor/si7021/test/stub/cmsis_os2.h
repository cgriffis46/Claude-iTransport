/*
 * Stand-in for the real cmsis_os2.h so xbmp280.h builds on a host.
 * osDelay() only records what it was asked for.
 */
#ifndef CMSIS_OS2_STUB_H_
#define CMSIS_OS2_STUB_H_

#include <stdint.h>
#include <vector>

extern uint32_t g_slept;
extern std::vector<uint32_t> g_delays;

inline int osDelay(uint32_t ticks) {
	g_slept += ticks;
	g_delays.push_back(ticks);
	return 0;
}

#endif
