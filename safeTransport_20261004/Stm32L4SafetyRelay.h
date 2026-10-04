#pragma once
// NOTE: this project's stub/example header has been "stm32f4xx_hal.h"
// throughout for consistency with everything else built so far. The
// REAL target here is STM32L432KC (L4 series) — a real project would
// include "stm32l4xx_hal.h" instead. L4 (unlike L5/G4/H7) uses the
// same classic bxCAN peripheral as F4, so Stm32HalCanTransport should
// carry over; IWDG/UART/task-notification usage below is standard
// HAL/FreeRTOS API, not F4-specific either way.
#include "stm32f4xx_hal.h"
#include "FreeRTOS.h"
#include "task.h"
#include "Safe.h"
#include "Stm32HalUartTransport.h"

// L432KC-specific safety relay core: two dedicated loopback UARTs,
// each carrying a 32-bit serial number plus a CRC-8, transmitted as a
// fixed 5-byte message (4 bytes serial number, LSB first, then 1 byte
// CRC-8 over those 4 bytes), out and back.
//
// The CRC is used entirely internally: the SAME computeCrc8() both
// generates it when building the outgoing message and validates it
// when checking a received one, so it doesn't need to match any
// external/named CRC-8 standard's exact test vectors to be correct —
// it only needs to reliably catch corruption between transmission and
// reception, which is verified directly against this class's own
// send/receive pair, not an external reference.
//
// A 32-bit serial number + CRC is a real improvement over a single
// fixed byte for the stuck-at-fault concern noted when this class
// only sent one byte: a fault that happens to freeze the line at a
// value that STILL passes CRC validation is far less likely across 5
// bytes than 1. It does NOT fully eliminate that concern, though —
// this is still a FIXED value every round. Only a value that
// genuinely changes round to round (an incrementing counter, say)
// would close that gap completely; not implemented here, since a
// fixed serial number is what was specifically described.
//
// Rather than periodic poll()-based timeout tracking (UartLoopbackSafe's
// approach), this uses native FreeRTOS task notifications: each
// channel's ISR-side sink accumulates received bytes into a per-
// channel 5-byte buffer and, ONLY once all 5 bytes have arrived AND
// both the serial number and the CRC check out, notifies the waiting
// task, setting that channel's bit. The main thread blocks in
// xTaskNotifyWait(), accumulating both channels' bits across however
// many notification events arrive within a round, and feeds the
// STM32's independent watchdog (IWDG) only once BOTH channels have
// confirmed within that round's timeout. A corrupted byte, a wrong
// serial number, a failed CRC check, and a message that never fully
// arrives all lead to the SAME outcome: that channel's bit is never
// set, the round times out, the watchdog isn't fed, and the hardware
// watchdog's own timeout eventually resets the MCU.
//
// KNOWN OPEN ISSUE, worth treating seriously: if a round times out
// with a PARTIAL message still in flight (say, 3 of 5 bytes received)
// and the next round immediately resets that channel's byte counter,
// a late-arriving byte from the timed-out round could get mixed into
// the new round's accumulation, corrupting it in a confusing way
// rather than failing cleanly. Mitigating this properly (a settle
// window between rounds, or a per-round sequence check) isn't
// implemented here — flagging it rather than silently assuming
// roundTimeoutTicks_ is always generous enough that this can't happen.
//
// This is a genuinely different, hardware-enforced failsafe compared
// to UartLoopbackSafe's software-only "just don't report Safe": a
// runaway, hung, or otherwise misbehaving MCU that stops running this
// logic entirely also stops feeding the watchdog, and gets reset
// regardless of whether any software is left running to notice.
//
// Native FreeRTOS task notifications (xTaskNotifyFromISR()/
// xTaskNotifyWait()), not CMSIS-RTOS2 — same kind of deliberate,
// scoped exception as xBNO085's stream buffer/queue usage.
class Stm32L4SafetyRelay : public Safe {
public:
    // Message layout: 4 bytes serial number (LSB first) + 1 byte
    // CRC-8 (polynomial 0x07) over those 4 bytes.
    static constexpr size_t kMessageLen = 5;

    // Valid range for watchdogTimeoutMs below, as specified.
    static constexpr uint32_t kMinWatchdogTimeoutMs = 1;
    static constexpr uint32_t kMaxWatchdogTimeoutMs = 15;

    // huart1/huart2 must already be configured via CubeMX (matching
    // Stm32HalUartTransport's own requirement). hiwdg must be a
    // constructed-but-NOT-yet-initialized IWDG_HandleTypeDef with its
    // ->Instance field already set (e.g. IWDG) via CubeMX — this
    // constructor itself computes the prescaler/reload and calls
    // HAL_IWDG_Init(), unlike the earlier version of this class, which
    // assumed the watchdog was already configured externally and only
    // ever called HAL_IWDG_Refresh(). watchdogTimeoutMs is clamped to
    // [kMinWatchdogTimeoutMs, kMaxWatchdogTimeoutMs] (1-15ms, as
    // specified) — see computeIwdgReload()'s own comment for the
    // real precision caveat that comes with a window this tight.
    // serialNumber is the fixed 32-bit value transmitted and expected
    // back on BOTH channels; the outgoing 5-byte message (serial
    // number + CRC) is built once here, not recomputed every round,
    // since the value itself never changes round to round.
    // roundTimeoutTicks bounds how long a single verification round
    // waits for both channels to fully confirm before giving up (and
    // not feeding the watchdog) for that round — this should be
    // chosen with watchdogTimeoutMs in mind: a verification round
    // that legitimately takes longer than the watchdog's own timeout
    // to succeed will get reset before it ever has a chance to feed
    // the watchdog, regardless of whether the loopback hardware is
    // actually healthy.
    Stm32L4SafetyRelay(UART_HandleTypeDef* huart1, UART_HandleTypeDef* huart2,
                        IWDG_HandleTypeDef* hiwdg, uint32_t serialNumber,
                        uint32_t roundTimeoutTicks, uint32_t watchdogTimeoutMs);

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

    // Must be called ONCE, from the task that will call
    // runVerificationRound() below — captures that task's handle
    // (xTaskGetCurrentTaskHandle()) as the notification target the
    // ISR-side sinks will notify. NOT done in the constructor: the
    // scheduler may not be running yet (or this task may not yet be
    // the calling context) at construction time — same reasoning as
    // SensorBase's own begin()/startThread() split elsewhere in this
    // codebase.
    void begin();

    // Call this repeatedly from the main thread's own loop (the same
    // task that called begin()). Resets both channels' receive state,
    // transmits the 5-byte message on both channels, blocks (via
    // xTaskNotifyWait()) accumulating both channels' confirmation
    // bits across however many notifications arrive, until either
    // both have arrived or roundTimeoutTicks elapses, then feeds the
    // watchdog if — and only if — both fully validated within this round.
    void runVerificationRound();

    // Standard CRC-8, polynomial 0x07, init 0x00, no reflection.
    // Exposed (not just a private helper) so it can be verified
    // directly — see this class's own header comment for why external
    // standard conformance doesn't matter here, only send/receive
    // consistency and genuine error-detection.
    static uint8_t computeCrc8(const uint8_t* data, size_t len);

    // Computes the IWDG reload register value (with the prescaler
    // fixed at /4, IWDG_PRESCALER_4) needed for a target timeout of
    // timeoutMs, clamped to [kMinWatchdogTimeoutMs, kMaxWatchdogTimeoutMs].
    //
    // t_IWDG(ms) = 4 * 2^PR * (RLR+1) / f_LSI(kHz); with PR=0 (÷4)
    // fixed, RLR = round(timeoutMs * f_LSI_kHz / 4) - 1. Assumes a
    // NOMINAL LSI frequency of 32 kHz — verify this against the
    // actual L432KC datasheet/reference manual before relying on it.
    //
    // REAL PRECISION CAVEAT, not a formality: LSI is an internal RC
    // oscillator, not a crystal — it has genuine run-to-run and
    // temperature-dependent tolerance (commonly several percent, and
    // it can be more under some conditions), not just a fixed,
    // knowable offset from nominal. For a wide watchdog window (say,
    // hundreds of milliseconds), a few percent of variance is
    // negligible. For a 1-15ms window, that same percentage is a much
    // larger fraction of the target value — the ACTUAL timeout this
    // produces on real silicon will differ from the nominal
    // calculation here by whatever this specific chip's LSI happens
    // to be running at, at whatever temperature it's at. Exposed here
    // (not just inlined into the constructor) specifically so the
    // reload value it produces can be checked and reasoned about
    // independently of construction.
    static uint32_t computeIwdgReload(uint32_t timeoutMs);

private:
    // Each channel claims a distinct 16-bit half of the 32-bit
    // notification value, rather than a single bit — channel 1 the
    // lower half, channel 2 the upper. xTaskNotifyFromISR(eSetBits)
    // ORs a channel's full pattern in; the round only completes once
    // BOTH halves are set (bitsReceived == kBothBits, all 32 bits).
    // Real, if modest, benefit over one bit each: any stray write
    // elsewhere in the firmware that accidentally touches bit 0 or
    // bit 1 via eSetBits would look exactly like a legitimate channel
    // confirmation with the old scheme. Requiring a full, specific
    // 16-bit pattern per channel makes an ACCIDENTAL collision from
    // unrelated code astronomically less likely. This is NOT
    // protection against a deliberately malicious actor, nor does it
    // catch an ISR that's genuinely buggy in a way that writes the
    // wrong channel's own pattern — it specifically rules out
    // accidental collisions from code elsewhere in the system.
    static constexpr uint32_t kChannel1Bit = 0x0000FFFFu;
    static constexpr uint32_t kChannel2Bit = 0xFFFF0000u;
    static constexpr uint32_t kBothBits    = kChannel1Bit | kChannel2Bit; // 0xFFFFFFFF

    // Per-channel receive accumulation state — each channel needs its
    // own, since bytes from channel 1 and channel 2 can arrive
    // interleaved in time and must never be mixed into the same buffer.
    struct ChannelRxState {
        uint8_t buffer[kMessageLen] = {0};
        size_t  bytesReceived = 0;
    };

    class ChannelSink : public iTransportRxSink {
    public:
        ChannelSink(Stm32L4SafetyRelay& owner, uint32_t bit) : owner_(owner), bit_(bit) {}
        void onByteReceived(uint8_t byte) override { owner_.onChannelByteReceived(bit_, byte); }
    private:
        Stm32L4SafetyRelay& owner_;
        uint32_t bit_;
    };

    void onChannelByteReceived(uint32_t bit, uint8_t byte); // ISR context
    void reportChannelState(bool& stateField, SafeCallback callback, void* context, bool newState);

    Stm32HalUartTransport transport1_;
    Stm32HalUartTransport transport2_;
    ChannelSink            sink1_;
    ChannelSink            sink2_;

    IWDG_HandleTypeDef* hiwdg_;
    uint32_t             serialNumber_;
    uint8_t              txMessage_[kMessageLen]; // built once, in the constructor
    uint32_t             roundTimeoutTicks_;
    TaskHandle_t         waitingTask_ = nullptr;

    ChannelRxState channel1Rx_;
    ChannelRxState channel2Rx_;

    bool         safe1_ = false;
    bool         safe2_ = false;
    SafeCallback safe1Callback_ = nullptr;
    SafeCallback safe2Callback_ = nullptr;
    void*        safe1CallbackContext_ = nullptr;
    void*        safe2CallbackContext_ = nullptr;
};
