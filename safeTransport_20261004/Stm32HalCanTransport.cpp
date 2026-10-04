#include "Stm32HalCanTransport.h"
#include <cstddef>

namespace {
    // Same fixed registry pattern as I2CTransport/Stm32HalUartTransport
    // — HAL_CAN_RxFifo0MsgPendingCallback is a global weak callback
    // keyed by CAN_HandleTypeDef*, not per-instance.
    constexpr size_t kMaxCanBuses = 2;
    CAN_HandleTypeDef*    s_handle[kMaxCanBuses]   = {nullptr, nullptr};
    Stm32HalCanTransport* s_activeOn[kMaxCanBuses] = {nullptr, nullptr};
}

void Stm32HalCanTransport::registerActive(CAN_HandleTypeDef* hcan, Stm32HalCanTransport* instance) {
    for (size_t i = 0; i < kMaxCanBuses; ++i) {
        if (s_handle[i] == nullptr || s_handle[i] == hcan) {
            s_handle[i]   = hcan;
            s_activeOn[i] = instance;
            return;
        }
    }
    // registry full — bump kMaxCanBuses if a project uses more than 2 CAN peripherals
}

void Stm32HalCanTransport::onRxFifo0MsgPending(CAN_HandleTypeDef* hcan) {
    for (size_t i = 0; i < kMaxCanBuses; ++i) {
        if (s_handle[i] == hcan && s_activeOn[i] != nullptr) {
            s_activeOn[i]->handleRxFifo0MsgPending();
            return;
        }
    }
}

Stm32HalCanTransport::Stm32HalCanTransport(CAN_HandleTypeDef* hcan, uint32_t txId, uint32_t rxFilterId)
    : hcan_(hcan), txId_(txId), rxFilterId_(rxFilterId) {
    registerActive(hcan_, this);

    // Exact-match filter: mask bits all 1 means every bit of the
    // incoming ID must match rxFilterId_ exactly, standard (11-bit)
    // IDs only, routed to RX FIFO 0.
    CAN_FilterTypeDef filter{};
    filter.FilterIdHigh         = static_cast<uint16_t>(rxFilterId_ << 5);
    filter.FilterIdLow          = 0;
    filter.FilterMaskIdHigh     = static_cast<uint16_t>(0x7FF << 5);
    filter.FilterMaskIdLow      = 0;
    filter.FilterFIFOAssignment = CAN_FILTER_FIFO0;
    filter.FilterBank           = 0;
    filter.FilterMode           = CAN_FILTERMODE_IDMASK;
    filter.FilterScale          = CAN_FILTERSCALE_32BIT;
    filter.FilterActivation     = CAN_FILTER_ENABLE;
    filter.SlaveStartFilterBank = 14;

    HAL_CAN_ConfigFilter(hcan_, &filter);
    HAL_CAN_Start(hcan_);
    HAL_CAN_ActivateNotification(hcan_, CAN_IT_RX_FIFO0_MSG_PENDING);
}

bool Stm32HalCanTransport::write(const uint8_t* data, size_t len) {
    if (len > 8) return false; // classic CAN's hard per-frame limit — no splitting here

    CAN_TxHeaderTypeDef txHeader{};
    txHeader.StdId = txId_;
    txHeader.IDE   = CAN_ID_STD;
    txHeader.RTR   = CAN_RTR_DATA;
    txHeader.DLC   = static_cast<uint32_t>(len);
    txHeader.TransmitGlobalTime = DISABLE;

    uint8_t txData[8] = {0};
    for (size_t i = 0; i < len; ++i) txData[i] = data[i];

    uint32_t txMailbox = 0;
    return HAL_CAN_AddTxMessage(hcan_, &txHeader, txData, &txMailbox) == HAL_OK;
}

// ISR context. Retrieves the pending frame and hands its data bytes
// to rxSink_ one at a time, if a sink has been attached — same
// "silently drop if no sink yet" convention as Stm32HalUartTransport.
// Deliberately does NOT check the frame's arbitration ID against
// rxFilterId_ here — the hardware filter configured in the
// constructor already guarantees only matching frames reach this
// callback at all.
void Stm32HalCanTransport::handleRxFifo0MsgPending() {
    CAN_RxHeaderTypeDef rxHeader{};
    uint8_t rxData[8] = {0};

    if (HAL_CAN_GetRxMessage(hcan_, CAN_RX_FIFO0, &rxHeader, rxData) != HAL_OK) {
        return;
    }

    if (rxSink_) {
        for (uint32_t i = 0; i < rxHeader.DLC; ++i) {
            rxSink_->onByteReceived(rxData[i]);
        }
    }
}
