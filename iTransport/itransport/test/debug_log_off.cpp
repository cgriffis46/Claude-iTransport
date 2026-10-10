// Part of debug_log_test: the macros at ITRANSPORT_DEBUG 0 must be empty
// and must not evaluate their arguments.
#undef ITRANSPORT_DEBUG
#define ITRANSPORT_DEBUG 0
#include "DebugLog.h"

int offSideEffects() {
    int n = 0;
    DBG_FAULT("off", "fault", ++n);
    DBG_EVENT("off", "event", ++n, ++n);
    DBG_TRACE("off", "trace", ++n);
    DBG_PIN(++n, true);
    DBG_PULSE(++n);
    return n;
}
