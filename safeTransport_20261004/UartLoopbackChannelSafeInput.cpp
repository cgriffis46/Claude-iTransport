#include "UartLoopbackChannelSafeInput.h"

UartLoopbackChannelSafeInput::UartLoopbackChannelSafeInput(iTransport& transport,
                                                             const uint8_t* serialNumber,
                                                             size_t serialNumberLen,
                                                             uint32_t timeoutTicks)
    : TransportSafeInput(transport), timeoutTicks_(timeoutTicks) {
    serialNumberLen_ = (serialNumberLen > kMaxSerialLen) ? kMaxSerialLen : serialNumberLen;
    for (size_t i = 0; i < serialNumberLen_; ++i) {
        serialNumber_[i] = serialNumber[i];
    }
}

void UartLoopbackChannelSafeInput::poll(uint32_t nowTicks) {
    switch (state_) {
    case ChannelState::Idle:
        // State/tracking fields must be set BEFORE issuing the write —
        // see UartLoopbackSafe's own pollChannel() comment for why
        // (an echo could in principle be observed extremely close to
        // when write() returns).
        bytesReceived_      = 0;
        mismatchDetected_   = false;
        state_              = ChannelState::WaitingForEcho;
        transmitStartTicks_ = nowTicks;
        if (!transport_.write(serialNumber_, serialNumberLen_)) {
            state_ = ChannelState::Idle; // failed to even issue the write — retry next poll()
        }
        break;

    case ChannelState::WaitingForEcho:
        if (bytesReceived_ >= serialNumberLen_) {
            const bool safe = !mismatchDetected_;
            setSafe1State(safe);
            setSafe2State(safe); // same value on both — see this class's header comment
            state_ = ChannelState::Idle;
        } else if (nowTicks - transmitStartTicks_ >= timeoutTicks_) {
            setSafe1State(false);
            setSafe2State(false);
            state_ = ChannelState::Idle;
        }
        break;
    }
}

// ISR context.
void UartLoopbackChannelSafeInput::onByteReceived(uint8_t byte) {
    if (state_ != ChannelState::WaitingForEcho) return; // stray/unexpected byte — ignore

    if (bytesReceived_ < serialNumberLen_) {
        if (byte != serialNumber_[bytesReceived_]) {
            mismatchDetected_ = true;
        }
        ++bytesReceived_;
    }
}
