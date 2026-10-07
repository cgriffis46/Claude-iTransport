#pragma once
#include "iTransport.h"

// Marker subclass: an iTransport specifically backed by a CAN bus.
// Deliberately minimal — no CAN-specific methods (arbitration ID,
// frame type, bus-off recovery, etc.) added yet, since none have been
// asked for. This exists so a concrete CAN transport can be typed as
// "an iTransport that's specifically CAN" (SafeTransport<iTransportCan>,
// say) rather than only the fully generic iTransport, leaving room to
// add real CAN-specific methods here later without disturbing
// anything that only needs the base iTransport contract.
class iTransportCan : public iTransport {
public:
    ~iTransportCan() override = default;
};
