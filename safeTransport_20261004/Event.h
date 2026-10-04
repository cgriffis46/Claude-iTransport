#pragma once
#include <cstdint>
#include <cstddef>
#include "EventCode.h"

// A single event: a code identifying WHAT happened, plus optional,
// small, fixed-size context data — deliberately no dynamic
// allocation, matching this project's established convention for
// anything that might live on constrained embedded firmware (a
// drone's own MCU) as well as server software. kMaxDataLen is
// intentionally small; an event is a notification, not a payload
// channel — something that needs to carry a real data block should
// use a SafeInput/SafeOutput or a dedicated transport instead.
struct Event {
    static constexpr size_t kMaxDataLen = 8;

    EventCode code      = CommonEventCodes::Unknown;
    uint32_t  timestamp = 0; // producer's own clock; meaning is producer-specific
    uint8_t   data[kMaxDataLen] = {0};
    size_t    dataLen   = 0;
};
