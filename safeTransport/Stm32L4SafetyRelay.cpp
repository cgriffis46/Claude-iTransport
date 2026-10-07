#include "Stm32L4SafetyRelay.h"

// Standard CRC-8, polynomial 0x07, init 0x00, no input/output
// reflection — a simple, bit-by-bit implementation, deliberately not
// a lookup table: for a 4-byte message, the speed difference is
// irrelevant (this easily fits an ISR's time budget either way), and
// a straightforward implementation is easier to visually verify than
// a generated table, which matters more here than raw speed.
uint8_t Stm32L4SafetyRelay::computeCrc8(const uint8_t* data, size_t len) {
    uint8_t crc = 0x00;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            if (crc & 0x80) {
                crc = static_cast<uint8_t>((crc << 1) ^ 0x07);
            } else {
                crc = static_cast<uint8_t>(crc << 1);
            }
        }
    }
    return crc;
}

// Nominal LSI frequency assumed for the timeout calculation below —
// verify against the actual L432KC datasheet/reference manual. See
// this method's own declaration in the header for the real precision
// caveat that comes with this: LSI's run-to-run and temperature
// tolerance is a much larger fraction of a 1-15ms target than it
// would be of a wider window.
namespace {
    constexpr uint32_t kAssumedLsiFrequencyHz = 32000;
}

uint32_t Stm32L4SafetyRelay::computeIwdgReload(uint32_t timeoutMs) {
    if (timeoutMs < kMinWatchdogTimeoutMs) timeoutMs = kMinWatchdogTimeoutMs;
    if (timeoutMs > kMaxWatchdogTimeoutMs) timeoutMs = kMaxWatchdogTimeoutMs;

    // t_IWDG(ms) = 4 * 2^PR * (RLR+1) / f_LSI(kHz), PR=0 (÷4) fixed:
    //   RLR = round(timeoutMs * f_LSI_kHz / 4) - 1
    const uint32_t lsiKHz = kAssumedLsiFrequencyHz / 1000;
    uint32_t reload = (timeoutMs * lsiKHz + 2) / 4; // +2 for round-to-nearest before the /4
    if (reload == 0) reload = 1; // RLR is defined down to 0, but 0 gives an unusably short window
    --reload;
    if (reload > 0xFFF) reload = 0xFFF; // IWDG_RLR is a 12-bit register
    return reload;
}

Stm32L4SafetyRelay::Stm32L4SafetyRelay(UART_HandleTypeDef* huart1, UART_HandleTypeDef* huart2,
                                        IWDG_HandleTypeDef* hiwdg, uint32_t serialNumber,
                                        uint32_t roundTimeoutTicks, uint32_t watchdogTimeoutMs)
    : transport1_(huart1), transport2_(huart2),
      sink1_(*this, kChannel1Bit), sink2_(*this, kChannel2Bit),
      hiwdg_(hiwdg), serialNumber_(serialNumber), roundTimeoutTicks_(roundTimeoutTicks) {
    transport1_.setRxSink(sink1_);
    transport2_.setRxSink(sink2_);

    // Configure and start the watchdog with the requested timeout —
    // unlike the earlier version of this class, hiwdg is not assumed
    // to already be initialized.
    hiwdg_->Init.Prescaler = IWDG_PRESCALER_4;
    hiwdg_->Init.Reload    = computeIwdgReload(watchdogTimeoutMs);
    HAL_IWDG_Init(hiwdg_);

    // Build the outgoing message ONCE — serialNumber_ never changes
    // round to round, so there's no need to recompute this every time
    // runVerificationRound() runs.
    txMessage_[0] = static_cast<uint8_t>(serialNumber_ & 0xFF);
    txMessage_[1] = static_cast<uint8_t>((serialNumber_ >> 8) & 0xFF);
    txMessage_[2] = static_cast<uint8_t>((serialNumber_ >> 16) & 0xFF);
    txMessage_[3] = static_cast<uint8_t>((serialNumber_ >> 24) & 0xFF);
    txMessage_[4] = computeCrc8(txMessage_, 4);
}

void Stm32L4SafetyRelay::begin() {
    waitingTask_ = xTaskGetCurrentTaskHandle();
}

// ISR context — called by Stm32HalUartTransport's own RX-complete ISR
// via the attached ChannelSink. Accumulates bytes into that channel's
// own 5-byte buffer; once all 5 have arrived, validates BOTH the
// serial number (must exactly match serialNumber_) AND the CRC (must
// match what computeCrc8() produces over the received 4 bytes) before
// notifying — any mismatch in either check means this channel's bit
// simply never gets set this round, leading to the same timed-out,
// watchdog-not-fed outcome as a byte that never arrives at all.
//
// xTaskNotifyFromISR(), not xTaskNotify() — see this class's header
// comment for why the plain (non-ISR) call would be incorrect here.
void Stm32L4SafetyRelay::onChannelByteReceived(uint32_t bit, uint8_t byte) {
    ChannelRxState& rx = (bit == kChannel1Bit) ? channel1Rx_ : channel2Rx_;

    if (rx.bytesReceived >= kMessageLen) {
        return; // already complete this round — ignore any extra/stray bytes
    }

    rx.buffer[rx.bytesReceived] = byte;
    ++rx.bytesReceived;

    if (rx.bytesReceived < kMessageLen) {
        return; // message not complete yet — wait for more bytes
    }

    // All 5 bytes are in — validate.
    uint32_t receivedSerial = static_cast<uint32_t>(rx.buffer[0]) |
                               (static_cast<uint32_t>(rx.buffer[1]) << 8) |
                               (static_cast<uint32_t>(rx.buffer[2]) << 16) |
                               (static_cast<uint32_t>(rx.buffer[3]) << 24);
    const uint8_t expectedCrc = computeCrc8(rx.buffer, 4);
    const uint8_t receivedCrc = rx.buffer[4];

    if (receivedSerial == serialNumber_ && receivedCrc == expectedCrc && waitingTask_ != nullptr) {
        BaseType_t higherPriorityTaskWoken = pdFALSE;
        xTaskNotifyFromISR(waitingTask_, bit, eSetBits, &higherPriorityTaskWoken);
        portYIELD_FROM_ISR(higherPriorityTaskWoken);
    }
    // else: wrong serial number, or failed CRC — do not notify. This
    // channel's bit is never set this round, same outcome as if the
    // message had never arrived at all.
}

void Stm32L4SafetyRelay::reportChannelState(bool& stateField, SafeCallback callback,
                                             void* context, bool newState) {
    const bool changed = (stateField != newState);
    stateField = newState;
    if (changed && callback) callback(*this, context);
}

void Stm32L4SafetyRelay::runVerificationRound() {
    // Reset both channels' receive state BEFORE transmitting — see
    // this class's header comment on the known late-arriving-byte
    // edge case this reset doesn't fully protect against.
    channel1Rx_.bytesReceived = 0;
    channel2Rx_.bytesReceived = 0;

    transport1_.write(txMessage_, kMessageLen);
    transport2_.write(txMessage_, kMessageLen);

    uint32_t bitsReceived = 0;
    const TickType_t deadline = xTaskGetTickCount() + roundTimeoutTicks_;

    while (bitsReceived != kBothBits) {
        const TickType_t now = xTaskGetTickCount();
        if (now >= deadline) break; // round timed out — not both channels confirmed in time

        uint32_t notifiedValue = 0;
        xTaskNotifyWait(0, kBothBits, &notifiedValue, deadline - now);
        bitsReceived |= notifiedValue;
    }

    if (bitsReceived == kBothBits) {
        HAL_IWDG_Refresh(hiwdg_);
    }
    // else: deliberately do NOT feed the watchdog — if this keeps
    // failing, the hardware watchdog's own timeout resets the MCU,
    // independent of whether any of this software is still running
    // to notice or report it.

    // Exact-match, not any-overlap: a single stray bit anywhere in a
    // channel's 16-bit region must NOT count as that channel
    // confirming — that's the entire point of widening the pattern
    // from a single bit in the first place. (bitsReceived & kChannelNBit)
    // != 0 would satisfy on ANY nonzero overlap, defeating that
    // purpose; == kChannelNBit requires every bit in that channel's
    // region to be set.
    reportChannelState(safe1_, safe1Callback_, safe1CallbackContext_, (bitsReceived & kChannel1Bit) == kChannel1Bit);
    reportChannelState(safe2_, safe2Callback_, safe2CallbackContext_, (bitsReceived & kChannel2Bit) == kChannel2Bit);
}
