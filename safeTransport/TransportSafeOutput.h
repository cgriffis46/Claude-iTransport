#pragma once
#include "SafeOutput.h"
#include "iTransport.h"

// A SafeOutput specifically backed by an iTransport — same reasoning
// as TransportSafeInput. NOT every SafeOutput needs this (a
// GPIO-backed one has no transport at all and derives from SafeOutput
// directly instead).
class TransportSafeOutput : public SafeOutput {
public:
    explicit TransportSafeOutput(iTransport& transport) : transport_(transport) {}

    // send() stays pure virtual, inherited from SafeOutput — HOW to
    // encode and transmit is entirely protocol-specific even among
    // transport-backed outputs (a CAN frame's arbitration ID and
    // 8-byte payload vs a continuous byte stream are nothing alike).

protected:
    iTransport& transport_;
};
