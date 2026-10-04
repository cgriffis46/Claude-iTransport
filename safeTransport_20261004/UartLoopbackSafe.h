#pragma once
#include <cstdint>
#include <cstddef>
#include "Safe.h"
#include "iTransport.h"

// UART-loopback-based Safe: verifies each channel by transmitting a
// known serial number out that channel's UART and confirming the
// EXACT same bytes loop back within a timeout — physically proving
// the loopback path (whatever it actually is: a wire, a jumper, a
// second UART's RX tied to the first's TX) is intact, and the UART
// hardware on both ends of it is actually working.
//
// This is a CONTINUOUS check, not a one-time startup test: call
// poll() periodically (e.g. from a dedicated task or timer) to
// re-verify — a wire coming loose AFTER the first successful check
// should still be caught. That's the whole point of a safety check,
// as opposed to a boot-time self-test.
//
// Each channel needs its OWN iTransport& — genuinely independent
// hardware paths, matching the dual-channel, diverse-fault-detection
// intent of the Safe interface itself. Never optimistically safe:
// both channels default to false until a full, successful round trip
// has actually been observed.
//
// KNOWN OPEN ISSUE, worth treating seriously given what this class
// is for: onChannelByteReceived() runs in ISR context (called from
// the transport's own receive interrupt) and writes bytesReceived_/
// mismatchDetected_ on each Channel; pollChannel() runs in
// whatever task calls poll() and reads those same fields. Nothing
// here currently enforces a memory barrier or critical section
// between the two — on a single-core Cortex-M this won't tear a
// single word, but there's no guarantee the compiler or hardware
// won't reorder the multi-field updates relative to poll()'s reads.
// For most sensor drivers in this codebase that's been an accepted,
// low-consequence tradeoff; for a safety-relevant class it deserves
// more deliberate treatment (volatile qualifiers at minimum, or a
// short critical section around the shared fields) before this is
// trusted for anything real. Flagged here rather than silently
// shipped as already-solved.
class UartLoopbackSafe : public Safe {
public:
    static constexpr size_t kMaxSerialLen = 32;

    // channel1Uart/channel2Uart must outlive this object; each is
    // wired up (setRxSink()) internally by this constructor — the
    // caller just constructs the transport and hands it over, same as
    // any other iTransport consumer in this codebase.
    // serialNumber/serialNumberLen: the known byte pattern transmitted
    // and compared on both channels (truncated to kMaxSerialLen if
    // longer). timeoutTicks: how long to wait for a full, correct
    // echo before declaring that round unsafe and retrying.
    UartLoopbackSafe(iTransport& channel1Uart, iTransport& channel2Uart,
                      const uint8_t* serialNumber, size_t serialNumberLen,
                      uint32_t timeoutTicks);

    bool GetSafe1State() const override { return channel_[0].safe; }
    bool GetSafe2State() const override { return channel_[1].safe; }

    void SetSafe1Callback(SafeCallback callback, void* context = nullptr) override {
        channel_[0].callback = callback;
        channel_[0].callbackContext = context;
    }
    void SetSafe2Callback(SafeCallback callback, void* context = nullptr) override {
        channel_[1].callback = callback;
        channel_[1].callbackContext = context;
    }

    // Call periodically (e.g. every timeoutTicks/2 or so) with the
    // current tick count — same nowTicks convention as SensorBase's
    // own main(nowMs). Drives both channels' loopback verification
    // independently; a callback fires only when a channel's state
    // actually CHANGES, not on every successful re-verification.
    void poll(uint32_t nowTicks);

private:
    enum class ChannelState { Idle, WaitingForEcho };

    struct Channel {
        iTransport* transport = nullptr;
        ChannelState    state = ChannelState::Idle;
        uint32_t        transmitStartTicks = 0;
        size_t          bytesReceived = 0;
        bool            mismatchDetected = false;
        bool            safe = false; // never optimistically true
        SafeCallback    callback = nullptr;
        void*           callbackContext = nullptr;
    };

    // Each channel needs its own iTransportRxSink, since iTransportRxSink's
    // onByteReceived() carries no channel identity of its own — two
    // small adapters, one per channel, each forwarding to this class
    // with the channel index baked in by which one is calling.
    class ChannelSink : public iTransportRxSink {
    public:
        ChannelSink(UartLoopbackSafe& owner, int index) : owner_(owner), index_(index) {}
        void onByteReceived(uint8_t byte) override { owner_.onChannelByteReceived(index_, byte); }
    private:
        UartLoopbackSafe& owner_;
        int index_;
    };

    void pollChannel(int index, uint32_t nowTicks);
    void onChannelByteReceived(int index, uint8_t byte); // ISR context — see class comment above
    void setChannelSafe(int index, bool safe);

    uint8_t  serialNumber_[kMaxSerialLen] = {0};
    size_t   serialNumberLen_ = 0;
    uint32_t timeoutTicks_;

    Channel     channel_[2];
    ChannelSink sink_[2];
};
