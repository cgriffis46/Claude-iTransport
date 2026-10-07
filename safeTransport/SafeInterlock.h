#pragma once
#include "Safe.h"

// Combines a Safe's two independent channels into ONE overall
// decision — encoding a principle that nothing in this codebase
// enforced before this class existed: Safe1 and Safe2 must AGREE,
// not just "either one happens to say safe." If the two channels
// disagree, that disagreement IS a fault — the redundancy itself has
// been compromised (a relay contact welded shut on one channel while
// the other opened correctly, say) — and is arguably more serious
// than both channels agreeing the system is unsafe, since it means
// something is wrong with the SAFETY MECHANISM itself, not just the
// thing it's watching.
//
// This exists because a real, if easy to make, mistake already
// happened in this codebase without it: an earlier end-to-end example
// wiring GpioSafeOutput read Safe1's callback alone, silently
// ignoring Safe2 entirely. Every future consumer of a Safe should go
// through this class, not read either channel directly and hope the
// other agrees.
class SafeInterlock {
public:
    enum class Result {
        Safe,        // both channels agree: safe
        Unsafe,      // both channels agree: unsafe
        Discrepancy  // channels DISAGREE — the redundancy itself is compromised
    };

    static Result evaluate(const Safe& safe) {
        const bool s1 = safe.GetSafe1State();
        const bool s2 = safe.GetSafe2State();
        if (s1 != s2) return Result::Discrepancy;
        return s1 ? Result::Safe : Result::Unsafe;
    }

    // Convenience for callers that just need a single boolean
    // decision (e.g. "should this output be energized"). A
    // discrepancy is treated as UNSAFE here, not as safe — never
    // optimistically pick one channel over the other when they
    // disagree; see this class's own header comment for why.
    static bool isFullySafe(const Safe& safe) {
        return evaluate(safe) == Result::Safe;
    }
};
