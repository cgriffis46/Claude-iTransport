#pragma once
#include <cstdint>

// A lightweight, extensible identifier for a specific kind of event —
// deliberately NOT a fixed enum. Different domains (a drone swarm's
// "approaching no-fly zone," a safety relay's own fault codes,
// whatever else ends up using this) each need their own set of
// codes, and a single shared enum would force every domain to either
// share one global namespace or fork the type entirely. A plain
// integer, with named constant ranges defined separately per domain,
// is the same "numeric space with well-known named values, room for
// domain-specific codes" pattern CAN arbitration IDs and HTTP status
// codes both use.
using EventCode = uint16_t;

// A small set of codes every consumer of this system should
// recognize, regardless of domain — reserved here so no domain-
// specific range collides with them. Domain-specific codes (e.g. a
// drone swarm's "ApproachingNoFlyZone") should be defined in their
// own header, starting at some higher, clearly-documented offset
// (e.g. 1000+), not added here.
namespace CommonEventCodes {
    constexpr EventCode Unknown   = 0;
    constexpr EventCode Heartbeat = 1;
}
