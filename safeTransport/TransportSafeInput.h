#pragma once
#include "SafeInput.h"
#include "iTransport.h"

// A SafeInput specifically backed by an iTransport — the common case
// (CAN, Ethernet, RS485, UART...) where there genuinely IS a byte
// stream to receive. NOT every SafeInput needs this (a GPIO-backed
// one doesn't have any transport at all), which is why this plumbing
// lives here, one level below the fully generic SafeInput, rather
// than baked into SafeInput itself.
class TransportSafeInput : public SafeInput, public iTransportRxSink {
public:
    explicit TransportSafeInput(iTransport& transport) : transport_(transport) {
        // Safe here: a normal call on transport_, an already-fully-
        // constructed object — not virtual dispatch on *this* before
        // *this* has finished constructing. Same reasoning as
        // xBNO085's own setRxSink() call.
        transport_.setRxSink(*this);
    }

    // onByteReceived() stays pure virtual, inherited unimplemented
    // from iTransportRxSink — HOW to decode received bytes into a
    // safety decision is entirely protocol-specific.

protected:
    iTransport& transport_;
};
