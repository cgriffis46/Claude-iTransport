#pragma once

// Abstract interface for a dual-channel safety input — deliberately
// transport-agnostic, the same way ISensorTransport is. A concrete
// realization could be:
//   - a UART loopback: transmit a known serial number, compare
//     against whatever comes back on the same (or a second) UART
//   - a digital-IO loopback: drive an output pin, read back an input
//     pin wired to it
//   - a subscription to some OTHER safety system entirely (a fieldbus
//     safety protocol, a CAN-based safety message, a remote safety
//     controller) — nothing here assumes any particular mechanism.
//
// Two independent channels (Safe1/Safe2), not one: this mirrors the
// dual-channel architecture common in functional-safety designs
// (IEC 61508-style) — two DIVERSE, INDEPENDENT means of confirming
// "safe," so a single fault in one channel's own mechanism doesn't,
// by itself, produce a false "safe" reading. Whether/how a caller
// combines the two (both must agree, one backs up the other, etc.) is
// deliberately NOT this interface's concern — that policy decision
// belongs to whoever consumes both channels, not to this contract.
class Safe {
public:
    virtual ~Safe() = default;

    // True once this channel currently confirms a safe state; false
    // otherwise — unsafe, OR not yet determined. A concrete
    // implementation should default to false until it has actually
    // verified the channel (e.g. before the first loopback round-trip
    // completes) — never optimistically report safe before it's
    // genuinely been confirmed.
    virtual bool GetSafe1State() const = 0;
    virtual bool GetSafe2State() const = 0;

    // Deliberately does NOT carry the state value itself — same
    // reasoning as SensorBase's own NewDataCallback: the callback
    // should call GetSafe1State()/GetSafe2State() itself to read the
    // state at the exact moment it acts, rather than risk acting on a
    // value that's already gone stale between being captured and the
    // callback actually running. Plain function pointer + context,
    // not std::function, to avoid any heap allocation.
    using SafeCallback = void (*)(Safe& source, void* context);

    virtual void SetSafe1Callback(SafeCallback callback, void* context = nullptr) = 0;
    virtual void SetSafe2Callback(SafeCallback callback, void* context = nullptr) = 0;
};
