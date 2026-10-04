#pragma once
#include "stm32f4xx_hal.h"
#include "iTransportCan.h"

// Classic bxCAN (HAL_CAN_*) implementation of iTransportCan — targets
// F1/F2/F3/F4/F7/L4-family STM32 parts. NOT compatible with H7/G4/G0
// parts, which use the FDCAN peripheral (HAL_FDCAN_*) instead — a
// genuinely different API surface, not just a naming difference.
// Confirm which CAN peripheral family the target MCU actually has
// before using this class.
//
// write()/setRxSink() map onto CAN awkwardly in one real respect,
// worth restating here since it affects how this class behaves: CAN
// messages arrive as complete, atomic frames (up to 8 data bytes for
// classic CAN — no CAN-FD support here), not a continuous byte
// stream. onRxFifo0MsgPending() below pushes each received frame's
// data bytes through iTransportRxSink one byte at a time, same
// mechanism as Stm32HalUartTransport — but that discards the fact
// they arrived together, as one atomic unit, with a specific
// arbitration ID. Fine for a fixed-length, single-purpose message
// (like a safety-state frame, this class's actual first real use),
// where the consumer already knows to expect exactly N bytes; a
// poor fit if this ever needs to carry frames of varying purpose or
// length, since nothing here preserves which arbitration ID a given
// byte actually came from.
//
// txId/rxFilterId are fixed at construction, not part of write()'s
// own arguments — CAN's arbitration ID is a property of the
// configured hardware filter/mailbox setup, not something
// iTransport's write(data, len) has any concept of. This transport
// always transmits on txId and only ever receives frames matching
// rxFilterId (an exact-match filter — every other bit must match,
// no masking); anything else arriving on the bus is invisible to it.
//
// write() rejects any payload over 8 bytes (classic CAN's hard
// per-frame limit) rather than attempting to split it across
// multiple frames — no multi-frame reassembly protocol exists here;
// each write() must correspond to exactly one CAN frame.
class Stm32HalCanTransport : public iTransportCan {
public:
    // hcan must already be initialized (HAL_CAN_Init()) via CubeMX
    // before this is constructed. This constructor itself calls
    // HAL_CAN_ConfigFilter(), HAL_CAN_Start(), and
    // HAL_CAN_ActivateNotification(CAN_IT_RX_FIFO0_MSG_PENDING) — the
    // peripheral is live and receiving as soon as this object exists.
    // txId/rxFilterId are 11-bit standard CAN identifiers (0-0x7FF);
    // extended (29-bit) IDs are not supported by this class.
    Stm32HalCanTransport(CAN_HandleTypeDef* hcan, uint32_t txId, uint32_t rxFilterId);

    // Sends len bytes (0-8) as a single CAN data frame on txId.
    // Returns false immediately, without touching the hardware, if
    // len > 8 — there is no framing/splitting here to make a longer
    // payload fit. Returns false if HAL_CAN_AddTxMessage() itself
    // fails (e.g. all three TX mailboxes currently full).
    bool write(const uint8_t* data, size_t len) override;
    void setRxSink(iTransportRxSink& sink) override { rxSink_ = &sink; }

    // Call this from the project's HAL_CAN_RxFifo0MsgPendingCallback
    // for `hcan` — ISR context. Finds the instance registered against
    // this hcan and dispatches to it.
    static void onRxFifo0MsgPending(CAN_HandleTypeDef* hcan);

private:
    void handleRxFifo0MsgPending();

    CAN_HandleTypeDef* hcan_;
    uint32_t           txId_;
    uint32_t           rxFilterId_;
    iTransportRxSink*  rxSink_ = nullptr; // nullptr until setRxSink() is called

    static void registerActive(CAN_HandleTypeDef* hcan, Stm32HalCanTransport* instance);
};
