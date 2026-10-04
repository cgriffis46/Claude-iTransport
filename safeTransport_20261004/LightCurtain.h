#pragma once
#include "SafeDevice.h"

// A light curtain, modeled as a SafeDevice — a named, domain-specific
// type carrying the manual-reset latch a real light curtain needs
// (see the header comment on why that matters, and this class's
// change history below), rather than a new mechanism of its own.
//
// Constructor signature is deliberately IDENTICAL to SafeDevice's own
// — two independent SafeInput channels required, no single-input
// convenience overload. An earlier version of this class took a
// single SafeInput& and silently duplicated it as both input1 and
// input2, on the reasoning that many light curtains only expose one
// logical clear/breached signal. That convenience was removed: it's
// too easy to reach for the one-argument constructor out of
// convenience even when two genuinely independent OSSD channels ARE
// available, silently giving up SafeInterlock's discrepancy detection
// (a channel disagreement — one OSSD circuit stuck safe while the
// other correctly trips — being caught at all) without the caller
// ever having to make that choice explicitly. Now they do: if a
// specific light curtain genuinely has only one output, the caller
// passes the SAME SafeInput& for both input1 and input2 themselves —
// verified safe to do (SafeDevice's constructor registers the same
// callback/context pair on both references regardless of whether
// they're the same object) — rather than this class making that
// decision on their behalf.
//
// What this class still adds over using SafeDevice directly, given
// the identical signature: a self-documenting type at the call site
// (LightCurtain frontCurtain(...) reads as what it is, rather than a
// generic SafeDevice whose domain meaning lives only in a comment),
// and a place for genuinely light-curtain-specific behavior to live
// later if it's ever needed (muting zones, blanking, EDM checks) —
// without retrofitting every call site that already uses this type.
//
// The manual-reset latch itself is exactly SafeDevice's: a breach
// latches unsafe immediately and automatically; recovery requires an
// explicit reset() (or a resetInput transition, if provided), never
// happening just because the beam cleared on its own — matching how
// real light curtain installations actually behave, unlike a bare
// SafeInput wired directly into a SafeZone, which would recover the
// instant the beam cleared with no acknowledgment at all.
class LightCurtain : public SafeDevice {
public:
    // input1/input2/output must all outlive this object, as must
    // resetInput if provided — same lifetime contract as SafeDevice's
    // own constructor, which this simply forwards to. Pass the same
    // SafeInput& for both input1 and input2 if this specific light
    // curtain genuinely only exposes one signal; pass two distinct,
    // independent SafeInputs if it has two real OSSD channels.
    LightCurtain(SafeInput& input1, SafeInput& input2, SafeOutput& output, SafeInput* resetInput = nullptr)
        : SafeDevice(input1, input2, output, resetInput) {}
};
