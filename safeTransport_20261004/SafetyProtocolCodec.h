#pragma once
#include <cstdint>
#include <cstddef>
#include "Safe.h"

// Separates "how to encode/decode a safety value for a SPECIFIC
// protocol" from "how to move bytes" (iTransport). Without this, a
// class like Stm32CanSafetyBroadcaster has its protocol logic and its
// transport logic fused together — fine for one custom format on one
// bus, but it means supporting a second real protocol (openSAFETY,
// CIP Safety, PROFIsafe) on the SAME or a DIFFERENT transport means
// rewriting the whole class rather than swapping one piece.
//
// With this split: a SafeInput/SafeOutput pair holds an iTransport&
// AND a SafetyProtocolCodec&, independently. The same Ethernet
// iTransport could carry openSAFETY frames on one instance and
// something else entirely on another; the KR260 (or any safe zone
// controller) can run several such pairs side by side for genuinely
// different protocols simultaneously, without any protocol-specific
// code touching the transport layer at all.
//
// A concrete codec's encode()/decode() should return false rather
// than silently produce a plausible-looking but incorrect frame if
// it can't do the job correctly — see CipSafetyCodec for why that
// distinction matters in practice, not just in principle.
class SafetyProtocolCodec {
public:
    virtual ~SafetyProtocolCodec() = default;

    // Encodes source's current Safe1/Safe2 state into outFrame,
    // writing the actual length used into outLen (outFrame's buffer
    // size is the caller's responsibility to provide large enough).
    // Returns false if encoding isn't possible (e.g. this codec isn't
    // actually implemented yet).
    virtual bool encode(const Safe& source, uint8_t* outFrame, size_t& outLen) = 0;

    // Decodes a received frame into outSafe1/outSafe2. Returns false
    // if the frame is invalid (corrupt, wrong length, failed whatever
    // protocol-specific integrity check applies) OR if this codec
    // isn't actually implemented yet — a caller must treat false as
    // "no valid data," never fall back to some default.
    virtual bool decode(const uint8_t* frame, size_t len, bool& outSafe1, bool& outSafe2) = 0;
};
