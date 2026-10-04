#pragma once
#include <string>
#include <cstdint>
#include "Safe.h"

// One member of a SafeZone: the live Safe& actually being evaluated,
// plus the name and safety code the zone controller uses to remember
// and persist what this device IS. The safety code is a stable,
// numeric identifier — something a technician can look up in a
// manual, or a log can reference — independent of the object's
// address in memory, which obviously can't survive a restart or mean
// anything written into a saved file.
struct SafeZoneMember {
    Safe*       safe;
    std::string name;
    uint32_t    safetyCode;
};
