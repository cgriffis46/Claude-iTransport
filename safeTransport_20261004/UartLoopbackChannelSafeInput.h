#pragma once
#include <cstdint>
#include <cstddef>
#include "TransportSafeInput.h"

// ONE UART loopback channel, standing alone as its own SafeInput —
// this is UartLoopbackSafe's dual-channel verification logic, split
// so each channel becomes an independently usable SafeInput rather
// than being bundled together inside one class that owns both.
// Transmits a known byte pattern and confirms it loops back exactly,
// within a timeout, continuously re-verified via poll() — same
// mechanism as UartLoopbackSafe, just for a single channel.
//
// Single-channel convention: this reports the SAME value on both
// GetSafe1State() and GetSafe2State(), since there's only one
// physical signal here and Safe's own shape (two independent
// channels) doesn't have a natural "just one channel" mode. See
// SafeInput's own header comment for why this isn't the only
// reasonable interpretation, just the one used here.
class UartLoopbackChannelSafeInput : public TransportSafeInput {
public:
    static constexpr size_t kMaxSerialLen = 32;

    // transport must outlive this object. serialNumber/serialNumberLen:
    // the known byte pattern transmitted and compared (truncated to
    // kMaxSerialLen if longer). timeoutTicks: how long to wait for a
    // full, correct echo before declaring this round unsafe and
    // retrying.
    UartLoopbackChannelSafeInput(iTransport& transport, const uint8_t* serialNumber,
                                  size_t serialNumberLen, uint32_t timeoutTicks);

    // ISR context — called by the transport whenever a byte arrives.
    void onByteReceived(uint8_t byte) override;

    // Call periodically (e.g. every timeoutTicks/2 or so) with the
    // current tick count — drives the loopback verification, same
    // convention as UartLoopbackSafe's own poll().
    void poll(uint32_t nowTicks);

private:
    enum class ChannelState { Idle, WaitingForEcho };

    uint8_t  serialNumber_[kMaxSerialLen] = {0};
    size_t   serialNumberLen_ = 0;
    uint32_t timeoutTicks_;

    ChannelState state_ = ChannelState::Idle;
    uint32_t     transmitStartTicks_ = 0;
    size_t       bytesReceived_ = 0;
    bool         mismatchDetected_ = false;
};
