#pragma once
#include <vector>
#include "Safe.h"
#include "SafeInterlock.h"
#include "SafeZoneMember.h"

// Aggregates multiple Safe members into ONE overall zone-level safety
// decision — "if every member is safe, the zone is safe." This is the
// natural next layer above SafeDevice: a safety relay (SafeDevice) is
// "the most basic component," and a SafeZone is what a higher-level
// supervisor (the KR260 acting as an upstream safe zone controller,
// per the architecture described earlier in this project) would use
// to aggregate several of them into one zone-level picture.
//
// Holds Safe&, not SafeDevice& specifically: since SafeDevice,
// SafeInput, and SafeZone itself all derive from Safe, a zone can
// hold its own local SafeDevices, a SHARED external safety signal not
// scoped to any one member (a light curtain), or another SafeZone
// entirely — letting zones nest, so a shared "FloorZone" can be a
// single member of every robot's own zone, and a fault anywhere in it
// correctly cascades to everyone watching it, with no separate
// cross-zone propagation mechanism needed.
//
// Every member is stored with a NAME and a numeric SAFETY CODE
// (SafeZoneMember), required at addMember() time rather than left
// optional — a zone controller that's meant to remember its own
// devices and persist that registry (see SafeZoneJsonPersistence.h)
// can't do that job for a member nobody bothered to name. See
// SafeZoneMember.h for why the code exists as a separate, stable
// identifier rather than relying on the object's own identity.
//
// KNOWN, DELIBERATE SCOPE LIMIT, not silently resolved: this only
// implements a logical AND across members (every member must be safe
// for the zone to be safe). Real commercial safety zone controllers
// typically offer configurable/adjustable logic blocks per input (AND,
// OR, and sometimes richer combinations) so a zone's overall decision
// can be tuned to what a specific installation actually needs. Adding
// that is a natural next step (most likely a per-member or per-zone
// configurable Logic enum used in EvaluateSafe()) but is NOT
// implemented here — every member is required to be safe, full stop,
// until that's built.
//
// Holds pointers to externally-owned Safe instances, not owning them
// itself — same "inject, don't own" convention every other composing
// class in this codebase follows.
//
// EvaluateSafe() is explicit, not automatically reactive to each
// member's own callbacks — call it periodically, or whenever you
// specifically want to re-check the zone.
//
// Single-channel convention: both GetSafe1State() and GetSafe2State()
// report the SAME value — there's one meaningful aggregate decision
// here, not two independent redundant channels.
//
// An EMPTY zone (no members added yet) is NOT vacuously safe — see
// EvaluateSafe() below.
class SafeZone : public Safe {
public:
    SafeZone() = default;
    virtual ~SafeZone() = default;

    // safe must outlive this SafeZone. name and safetyCode are
    // required, not optional — see this class's own header comment
    // on why a zone controller meant to remember and persist its
    // devices can't do that job for an unnamed, uncoded member.
    void addMember(Safe& safe, const std::string& name, uint32_t safetyCode);

    // Read-only access to the current member list — used by
    // SafeZoneJsonPersistence to write out the zone's registry, and
    // generally useful for anything else that needs to enumerate
    // what a zone currently knows about.
    size_t memberCount() const { return members_.size(); }
    const SafeZoneMember& memberAt(size_t index) const { return members_[index]; }

    // Populates members via discovery, entirely transport-specific —
    // see this class's earlier header comment history; default is a
    // no-op, a concrete transport-specific subclass overrides it.
    virtual void DiscoverSafeDevices() {}

    // Checks every member currently in the zone via SafeInterlock
    // (never a raw GetSafe1State()/GetSafe2State() read) and updates
    // Safe1/Safe2 accordingly, firing callbacks on change. Currently
    // a strict AND across every member — see this class's own header
    // comment on the configurable-logic-block limitation.
    void EvaluateSafe();

    bool GetSafe1State() const override { return safe1_; }
    bool GetSafe2State() const override { return safe2_; }
    void SetSafe1Callback(SafeCallback callback, void* context = nullptr) override {
        safe1Callback_ = callback;
        safe1CallbackContext_ = context;
    }
    void SetSafe2Callback(SafeCallback callback, void* context = nullptr) override {
        safe2Callback_ = callback;
        safe2CallbackContext_ = context;
    }

private:
    void reportState(bool& stateField, SafeCallback callback, void* context, bool newState);

    std::vector<SafeZoneMember> members_;

    bool         safe1_ = false;
    bool         safe2_ = false;
    SafeCallback safe1Callback_ = nullptr;
    SafeCallback safe2Callback_ = nullptr;
    void*        safe1CallbackContext_ = nullptr;
    void*        safe2CallbackContext_ = nullptr;
};
