#pragma once
#include "iTransport.h"

// Marker subclass: an iTransport specifically backed by Ethernet.
// Deliberately minimal — no Ethernet-specific methods (addressing,
// port, protocol framing, etc.) added yet, since none have been asked
// for. Same reasoning as iTransportCan: exists so a concrete Ethernet
// transport can be typed as specifically Ethernet
// (SafeTransport<iTransportEthernet>, say) rather than only the fully
// generic iTransport, leaving room to add real Ethernet-specific
// methods here later without disturbing anything that only needs the
// base iTransport contract.
class iTransportEthernet : public iTransport {
public:
    ~iTransportEthernet() override = default;
};
