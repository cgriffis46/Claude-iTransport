#include "UartLoopbackSafe.h"

UartLoopbackSafe::UartLoopbackSafe(iTransport& channel1Uart, iTransport& channel2Uart,
                                    const uint8_t* serialNumber, size_t serialNumberLen,
                                    uint32_t timeoutTicks)
    : timeoutTicks_(timeoutTicks),
      sink_{ ChannelSink(*this, 0), ChannelSink(*this, 1) } {
    channel_[0].transport = &channel1Uart;
    channel_[1].transport = &channel2Uart;

    serialNumberLen_ = (serialNumberLen > kMaxSerialLen) ? kMaxSerialLen : serialNumberLen;
    for (size_t i = 0; i < serialNumberLen_; ++i) {
        serialNumber_[i] = serialNumber[i];
    }

    // Safe here: normal calls on already-fully-constructed transport
    // objects, not virtual dispatch on *this* before *this* is done
    // constructing. Same reasoning as xBNO085's own setRxSink() call.
    channel1Uart.setRxSink(sink_[0]);
    channel2Uart.setRxSink(sink_[1]);
}

void UartLoopbackSafe::poll(uint32_t nowTicks) {
    pollChannel(0, nowTicks);
    pollChannel(1, nowTicks);
}

void UartLoopbackSafe::pollChannel(int index, uint32_t nowTicks) {
    Channel& ch = channel_[index];

    switch (ch.state) {
    case ChannelState::Idle:
        // State/tracking fields must be set BEFORE issuing the write,
        // not after: an echo could in principle be observed extremely
        // close to when write() returns (this fake-transport test
        // caught exactly that with a synchronous echo), and
        // onChannelByteReceived() only accepts bytes while in
        // WaitingForEcho. Setting state first means any echo — no
        // matter how fast — is correctly recognized rather than
        // silently discarded as a stray byte.
        ch.bytesReceived      = 0;
        ch.mismatchDetected   = false;
        ch.state              = ChannelState::WaitingForEcho;
        ch.transmitStartTicks = nowTicks;
        if (!ch.transport->write(serialNumber_, serialNumberLen_)) {
            // Failed to even issue the write (e.g. transport busy) —
            // revert to Idle and retry next poll() call, rather than
            // waiting out a full timeout for a transfer that never started.
            ch.state = ChannelState::Idle;
        }
        break;

    case ChannelState::WaitingForEcho:
        if (ch.bytesReceived >= serialNumberLen_) {
            // Full echo received — each byte was already compared as
            // it arrived (see onChannelByteReceived), so no mismatch
            // flagged means every byte actually matched.
            setChannelSafe(index, !ch.mismatchDetected);
            ch.state = ChannelState::Idle; // re-verify again next cycle
        } else if (nowTicks - ch.transmitStartTicks >= timeoutTicks_) {
            // No full/correct echo within the allowed window.
            setChannelSafe(index, false);
            ch.state = ChannelState::Idle; // retry
        }
        break;
    }
}

// ISR context — see this class's own header comment on the shared-
// state synchronization caveat between here and pollChannel().
void UartLoopbackSafe::onChannelByteReceived(int index, uint8_t byte) {
    Channel& ch = channel_[index];
    if (ch.state != ChannelState::WaitingForEcho) return; // stray/unexpected byte — ignore

    if (ch.bytesReceived < serialNumberLen_) {
        if (byte != serialNumber_[ch.bytesReceived]) {
            // This channel has already failed this round — keep
            // tracking, don't bail early, so bytesReceived still
            // reaches the expected count and pollChannel() can move
            // on rather than waiting out the full timeout for no reason.
            ch.mismatchDetected = true;
        }
        ++ch.bytesReceived;
    }
    // else: more bytes than expected arrived — ignore the overflow;
    // the length mismatch alone means this can't be a valid echo.
}

void UartLoopbackSafe::setChannelSafe(int index, bool safe) {
    Channel& ch = channel_[index];
    const bool changed = (ch.safe != safe);
    ch.safe = safe;
    if (changed && ch.callback) {
        ch.callback(*this, ch.callbackContext);
    }
}
