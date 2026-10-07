#pragma once
#include "SafetyProtocolCodec.h"

// Placeholder for CIP Safety over EtherNet/IP — deliberately NOT a
// working implementation. This is an honest gap, not a bug to fix
// quietly later: CIP Safety's actual wire format (the ENIP
// encapsulation header's exact field layout, the Safety Validator
// object's message structure, which CRC polynomial(s) it actually
// uses, the time-coordination mechanism's exact framing) is not
// something reliably known from memory with the confidence this
// codebase's other protocol work (the CRC-8 work, the custom CAN
// frame format) was built with.
//
// The distinction that matters here: guessing at an external
// standard's exact byte layout risks producing something that
// compiles, passes its own self-consistency tests, and is SILENTLY
// INCOMPATIBLE with any real CIP Safety device — a worse outcome
// than having no implementation at all, since it would look correct
// right up until it's tested against real hardware. encode()/decode()
// below both return false unconditionally, on purpose, rather than
// emit a plausible-looking but unverified frame.
//
// What's needed to actually implement this: ODVA's CIP Networks
// Library, Volume 5 (CIP Safety) — the real specification, not a
// summary or a memory of one. Once that's available, this class's
// encode()/decode() get filled in against it, and verified the same
// way everything else in this codebase has been: real test vectors,
// not just "it compiles."
class CipSafetyCodec : public SafetyProtocolCodec {
public:
    bool encode(const Safe& source, uint8_t* outFrame, size_t& outLen) override {
        (void)source; (void)outFrame; (void)outLen;
        return false; // deliberately unimplemented — see class comment
    }

    bool decode(const uint8_t* frame, size_t len, bool& outSafe1, bool& outSafe2) override {
        (void)frame; (void)len; (void)outSafe1; (void)outSafe2;
        return false; // deliberately unimplemented — see class comment
    }
};
