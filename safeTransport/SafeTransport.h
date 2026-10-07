#pragma once
#include <type_traits>
#include "Safe.h"
#include "iTransport.h"

// Combines a transport (TTransport) with safety-state reporting
// (Safe). Template, rather than hardcoded to one specific transport —
// a caller picks whichever concrete transport interface actually fits
// the physical medium in question: iTransport itself for a generic
// byte-stream transport, or a more specific descendant like
// iTransportCan or iTransportEthernet, via
// SafeTransport<TheirTransportInterface>.
//
// TTransport is required to derive from iTransport — enforced below,
// not just documented — now that iTransport exists as a genuine
// common base for every stream-oriented transport in this codebase
// (UART, and, following the same shape, CAN/Ethernet/RS485/ASI).
// iTransport itself used to be named IUartTransport; renamed once it
// became clear this template needed to back non-UART transports too,
// and a name implying UART specifically no longer fit what it was
// actually being used for.
//
// Still no diamond, for any TTransport that satisfies the constraint
// below: Safe has no base class of its own, and iTransport (or any of
// its descendants) doesn't share an ancestor with Safe either — the
// "one set of overrides satisfies two unrelated bases" mechanism
// already proven for BMP280Sensor (SensorBase + Barometer + Altimeter
// + Thermometer) and xBMP280 (BMP280Sensor + xSensorBase) applies here
// regardless of which iTransport descendant is actually chosen.
//
// Purely a combining interface, same as Safe/Barometer/Altimeter/
// Thermometer: no state, no shared logic — a concrete class provides
// everything TTransport and Safe each require. Header-only, like
// every pure-interface class in this codebase; no corresponding .cpp.
template <typename TTransport>
class SafeTransport : public TTransport, public Safe {
    static_assert(std::is_base_of<iTransport, TTransport>::value,
                  "SafeTransport<TTransport> requires TTransport to derive "
                  "from iTransport (directly, like iTransport itself, or "
                  "via a descendant such as iTransportCan/iTransportEthernet).");

public:
    ~SafeTransport() override = default;
};
