#include "SafeZone.h"

void SafeZone::addMember(Safe& safe, const std::string& name, uint32_t safetyCode) {
    members_.push_back(SafeZoneMember{&safe, name, safetyCode});
}

void SafeZone::EvaluateSafe() {
    // An empty zone is NOT vacuously safe, so allSafe starts false
    // unless there's at least one member to actually check. Strict
    // AND across every member — see the header's own comment on the
    // configurable-logic-block limitation this doesn't yet address.
    bool allSafe = !members_.empty();
    for (const SafeZoneMember& member : members_) {
        if (!SafeInterlock::isFullySafe(*member.safe)) {
            allSafe = false;
            break; // one failing member is enough to know the zone isn't safe
        }
    }

    reportState(safe1_, safe1Callback_, safe1CallbackContext_, allSafe);
    reportState(safe2_, safe2Callback_, safe2CallbackContext_, allSafe);
}

void SafeZone::reportState(bool& stateField, SafeCallback callback, void* context, bool newState) {
    const bool changed = (stateField != newState);
    stateField = newState;
    if (changed && callback) callback(*this, context);
}
