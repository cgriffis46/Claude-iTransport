#include "DualChannelLink.h"
#include "CrcS3.h"
#include "DebugLog.h"

namespace {
constexpr uint16_t kCrcSeed = 0xFFFFu;
constexpr uint8_t  kKnownFlags = DualChannelLink::kFlagLoopbackGood | DualChannelLink::kFlagHearingYou;
}

DualChannelLink::DualChannelLink(iTransport& link, const Safe& ownChannel, uint32_t ownId,
                                 const Config& config)
    : link_(link), ownChannel_(ownChannel), ownId_(ownId), config_(config) {
    if (config_.periodTicks == 0) config_.periodTicks = 1;
    if (config_.framesToTrust == 0) config_.framesToTrust = 1;
    // Last, once the ring is ready for the first byte.
    link_.setRxSink(*this);
}

void DualChannelLink::encode(uint8_t* out, uint16_t seq, uint32_t id, uint8_t flags) {
    out[0] = kStart0;
    out[1] = kStart1;
    out[2] = static_cast<uint8_t>(seq);
    out[3] = static_cast<uint8_t>(seq >> 8);
    out[4] = static_cast<uint8_t>(id);
    out[5] = static_cast<uint8_t>(id >> 8);
    out[6] = static_cast<uint8_t>(id >> 16);
    out[7] = static_cast<uint8_t>(id >> 24);
    out[8] = flags;
    const uint16_t crc = CrcS3::compute(out + 2, 7, kCrcSeed);
    out[9]  = static_cast<uint8_t>(crc);
    out[10] = static_cast<uint8_t>(crc >> 8);
}

// Interrupt context. The only writer of rxHead_ and rxOverflows_.
void DualChannelLink::onByteReceived(uint8_t byte) {
    const uint32_t head = rxHead_.load(std::memory_order_relaxed);
    const uint32_t tail = rxTail_.load(std::memory_order_acquire);
    if (head - tail >= kRxRingSize) {
        rxOverflows_.store(rxOverflows_.load(std::memory_order_relaxed) + 1,
                           std::memory_order_release);
        return;
    }
    rxRing_[head & (kRxRingSize - 1)] = byte;
    rxHead_.store(head + 1, std::memory_order_release);
}

void DualChannelLink::poll(uint32_t nowTicks) {
    readQueuedBytes(nowTicks);

    if (heardAny_ && role_ != Role::Alone &&
        nowTicks - lastGoodTicks_ > config_.timeoutTicks) {
        ++stats_.timeouts;
        DBG_FAULT("dcl", "timeout", (int32_t)(nowTicks - lastGoodTicks_));
        fault();
        role_ = Role::Alone;
    }
    report();

    if (!sentAny_ || nowTicks - lastSendTicks_ >= config_.periodTicks) {
        sendHeartbeat(nowTicks);
    }
}

void DualChannelLink::readQueuedBytes(uint32_t nowTicks) {
    const uint32_t overflows = rxOverflows_.load(std::memory_order_acquire);
    if (overflows != rxOverflowsSeen_) {
        // Bytes were dropped: whatever is half assembled is not to be
        // trusted, and a frame has been lost.
        stats_.rxOverflows += overflows - rxOverflowsSeen_;
        DBG_FAULT("dcl", "rx-overflow", (int32_t)(overflows - rxOverflowsSeen_));
        rxOverflowsSeen_ = overflows;
        frameLen_ = 0;
        fault();
    }

    uint32_t       tail = rxTail_.load(std::memory_order_relaxed);
    const uint32_t head = rxHead_.load(std::memory_order_acquire);
    while (tail != head) {
        const uint8_t byte = rxRing_[tail & (kRxRingSize - 1)];
        ++tail;
        rxTail_.store(tail, std::memory_order_release);
        parseByte(byte, nowTicks);
    }
}

void DualChannelLink::parseByte(uint8_t byte, uint32_t nowTicks) {
    frame_[frameLen_++] = byte;
    tryFrame(nowTicks);
}

// Drops leading bytes until frame_ starts like a frame; checks it once
// it is whole. After a bad CRC it looks for a start inside the bytes
// it already has, so one corrupted byte costs one frame, not two.
void DualChannelLink::tryFrame(uint32_t nowTicks) {
    for (;;) {
        size_t drop = 0;
        if (frameLen_ >= 1 && frame_[0] != kStart0) {
            drop = 1;
        } else if (frameLen_ >= 2 && frame_[1] != kStart1) {
            drop = 1;
        } else if (frameLen_ == kFrameLen) {
            const uint16_t crc = static_cast<uint16_t>(frame_[9] | (frame_[10] << 8));
            if (crc == CrcS3::compute(frame_ + 2, 7, kCrcSeed)) {
                const uint16_t seq = static_cast<uint16_t>(frame_[2] | (frame_[3] << 8));
                const uint32_t id  = static_cast<uint32_t>(frame_[4]) |
                                     (static_cast<uint32_t>(frame_[5]) << 8) |
                                     (static_cast<uint32_t>(frame_[6]) << 16) |
                                     (static_cast<uint32_t>(frame_[7]) << 24);
                frameLen_ = 0;
                acceptFrame(seq, id, frame_[8], nowTicks);
                return;
            }
            ++stats_.crcErrors;
            DBG_FAULT("dcl", "crc");
            fault();
            drop = 1;
        }
        if (drop == 0) return;
        for (size_t i = drop; i < frameLen_; ++i) frame_[i - drop] = frame_[i];
        frameLen_ -= drop;
    }
}

void DualChannelLink::acceptFrame(uint16_t seq, uint32_t id, uint8_t flags, uint32_t nowTicks) {
    if (id == ownId_) {
        // Our own frames coming back: the link is wired to itself.
        ++stats_.ownIdFrames;
        DBG_FAULT("dcl", "own-id");
        fault();
        return;
    }
    if (flags & ~kKnownFlags) {
        ++stats_.badFlagFrames;
        DBG_FAULT("dcl", "bad-flags", flags);
        fault();
        return;
    }
    if (heardAny_ && id != partnerId_) {
        // A different board: start its sequence afresh.
        ++stats_.sequenceErrors;
        DBG_FAULT("dcl", "new-partner", (int32_t)partnerId_, (int32_t)id);
        fault();
    }

    if (haveSeq_ && seq == static_cast<uint16_t>(lastSeq_ + 1)) {
        ++inSequence_;
    } else {
        if (haveSeq_) {
            // A repeat, a gap or a jump back.
            ++stats_.sequenceErrors;
            DBG_FAULT("dcl", "seq", (uint16_t)(lastSeq_ + 1), seq);
            fault();
        }
        inSequence_ = 1;   // this frame is the new baseline
    }

    ++stats_.framesGood;
    haveSeq_         = true;
    lastSeq_         = seq;
    lastGoodTicks_   = nowTicks;
    heardAny_        = true;
    partnerId_       = id;
    partnerLoopGood_ = (flags & kFlagLoopbackGood) != 0;
    partnerHearsUs_  = (flags & kFlagHearingYou) != 0;
    role_            = (ownId_ < partnerId_) ? Role::Primary : Role::Secondary;
    healthy_         = inSequence_ >= config_.framesToTrust;

    // Report every frame, not just at the end of poll(): a frame that
    // says "loopback bad" followed by one that says "good" in the same
    // poll() must still be seen as a drop.
    report();
}

// Any fault: the partner's channel is unsafe at once, and needs
// framesToTrust frames in sequence to count again.
void DualChannelLink::fault() {
    haveSeq_    = false;
    inSequence_ = 0;
    healthy_    = false;
    report();
}

void DualChannelLink::report() {
    const bool safe = healthy_ && partnerLoopGood_ && partnerHearsUs_;
#if ITRANSPORT_DEBUG
    if (safe != dbgSafe_) {
        // Why, in one line: healthy, its loopback good, it hears us.
        DBG_EVENT("dcl", "partner", safe ? 1 : 0, healthy_ ? 1 : 0, partnerLoopGood_ ? 1 : 0, partnerHearsUs_ ? 1 : 0);
        if (!safe) DBG_PULSE(dbg::kPinFault);
        dbgSafe_ = safe;
    }
#endif
    setSafe1State(safe);
    setSafe2State(safe);   // one physical channel: the same on both, as SafeInput describes
}

void DualChannelLink::sendHeartbeat(uint32_t nowTicks) {
    uint8_t flags = 0;
    if (ownChannel_.GetSafe1State() && ownChannel_.GetSafe2State()) flags |= kFlagLoopbackGood;
    if (healthy_) flags |= kFlagHearingYou;

    uint8_t* buf = tx_[txNext_];
    encode(buf, txSeq_, ownId_, flags);
    if (!link_.write(buf, kFrameLen)) return;   // still sending the last one: try next poll()

    ++txSeq_;
    txNext_ ^= 1;
    ++stats_.framesSent;
    sentAny_       = true;
    lastSendTicks_ = nowTicks;
}
