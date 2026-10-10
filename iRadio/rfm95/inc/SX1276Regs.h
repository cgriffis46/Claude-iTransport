#pragma once
// Semtech SX1276/77/78/79 registers in LoRa mode (HopeRF RFM95/96/97/98
// modules), the ones the rfm95 driver uses.
//
// Taken from Semtech's own LoRaMac-node (src/radio/sx1276/
// sx1276Regs-LoRa.h, sx1276.c) and cross-checked against Sandeep
// Mistry's arduino-LoRa (LoRa.cpp); the two agree on everything here.
// Semtech's datasheet could not be downloaded here.
//
// Like the RFM69, the SX1276 sets bit 7 of the address to write
// (SPITransport::AddressBit::WriteHigh).

#include <stdint.h>

namespace sx1276 {

// ---- registers ----
const uint8_t kRegFifo              = 0x00;
const uint8_t kRegOpMode            = 0x01;
const uint8_t kRegFrfMsb            = 0x06;   // then Mid 0x07, Lsb 0x08
const uint8_t kRegPaConfig          = 0x09;
const uint8_t kRegOcp               = 0x0B;
const uint8_t kRegLna               = 0x0C;
const uint8_t kRegFifoAddrPtr       = 0x0D;
const uint8_t kRegFifoTxBaseAddr    = 0x0E;
const uint8_t kRegFifoRxBaseAddr    = 0x0F;
const uint8_t kRegFifoRxCurrentAddr = 0x10;   // 0x10..0x13 read together: current, mask, flags, count
const uint8_t kRegIrqFlagsMask      = 0x11;
const uint8_t kRegIrqFlags          = 0x12;
const uint8_t kRegRxNbBytes         = 0x13;
const uint8_t kRegPktSnrValue       = 0x19;   // then PktRssiValue 0x1A
const uint8_t kRegPktRssiValue      = 0x1A;
const uint8_t kRegModemConfig1      = 0x1D;
const uint8_t kRegModemConfig2      = 0x1E;
const uint8_t kRegSymbTimeoutLsb    = 0x1F;
const uint8_t kRegPreambleMsb       = 0x20;   // then Lsb 0x21
const uint8_t kRegPayloadLength     = 0x22;
const uint8_t kRegMaxPayloadLength  = 0x23;
const uint8_t kRegModemConfig3      = 0x26;
const uint8_t kRegDetectOptimize    = 0x31;
const uint8_t kRegInvertIq          = 0x33;
const uint8_t kRegHighBwOptimize1   = 0x36;
const uint8_t kRegDetectionThreshold = 0x37;
const uint8_t kRegSyncWord          = 0x39;
const uint8_t kRegHighBwOptimize2   = 0x3A;
const uint8_t kRegInvertIq2         = 0x3B;
const uint8_t kRegDioMapping1       = 0x40;
const uint8_t kRegVersion           = 0x42;
const uint8_t kRegPaDac             = 0x4D;

const uint8_t kVersion = 0x12;   // RegVersion of the SX1276 family

// ---- RegOpMode ----
const uint8_t kOpLoRa       = 0x80;   // LongRangeMode: only changes while in sleep
const uint8_t kOpSleep      = 0x00;
const uint8_t kOpStandby    = 0x01;
const uint8_t kOpTx         = 0x03;
const uint8_t kOpRxContinuous = 0x05;
const uint8_t kOpRxSingle   = 0x06;
const uint8_t kOpModeMask   = 0x07;

// ---- RegIrqFlags (write 1 to clear) ----
const uint8_t kIrqRxTimeout = 0x80;
const uint8_t kIrqRxDone    = 0x40;
const uint8_t kIrqCrcError  = 0x20;
const uint8_t kIrqValidHeader = 0x10;
const uint8_t kIrqTxDone    = 0x08;
const uint8_t kIrqAll       = 0xFF;

// ---- RegModemConfig1/2/3 ----
// Config1: bandwidth << 4 | coding rate << 1 | implicit header.
// Config2: spreading factor << 4 | RxPayloadCrcOn (0x04) | SymbTimeout(9:8).
// Config3: LowDataRateOptimize (0x08) | AgcAutoOn (0x04).
const uint8_t kCrcOn  = 0x04;
const uint8_t kLdro   = 0x08;
const uint8_t kAgcAuto = 0x04;

// ---- PA ----
const uint8_t kPaBoost     = 0x80;   // the RFM95's PA output (RFO is not wired on it)
const uint8_t kPaDacNormal = 0x84;   // up to +17 dBm
const uint8_t kPaDac20dBm  = 0x87;   // +18..+20 dBm
const uint8_t kOcpOn       = 0x20;

// ---- LNA: gain G1, boost on (HF port) ----
const uint8_t kLnaMaxGainBoost = 0x23;

// ---- I/Q inversion (LoRaWAN receives downlinks inverted) ----
// Semtech writes RegInvertIq read-modify-write; its other bits keep their
// reset value, 0x26 (RX inversion is bit 6, TX "not inverted" bit 0).
const uint8_t kInvertIqNormal   = 0x27;   // RX normal, TX normal
const uint8_t kInvertIqRx       = 0x67;   // RX inverted, TX normal
const uint8_t kInvertIqTx       = 0x26;   // RX normal, TX inverted
const uint8_t kInvertIq2Normal  = 0x1D;
const uint8_t kInvertIq2Inverted = 0x19;

// ---- Detection ----
// RegDetectOptimize and RegDetectionThreshold reset to their SF7..SF12
// values, and SF6 (which needs others) is not used, so the driver
// leaves them alone.

// ---- Errata 2.1: sensitivity at 500 kHz bandwidth (from Semtech's code) ----
const uint8_t kHighBw500Opt1 = 0x02;       // RegHighBwOptimize1 at 500 kHz
const uint8_t kHighBw500Opt2Hf = 0x64;     // RegHighBwOptimize2 at 500 kHz, above 525 MHz
const uint8_t kHighBwOtherOpt1 = 0x03;     // RegHighBwOptimize1 otherwise

// ---- DIO mapping (RegDioMapping1 bits 7:6 = DIO0, 5:4 = DIO1) ----
const uint8_t kDio0RxDone  = 0x00;   // DIO1 00: RxTimeout
const uint8_t kDio0TxDone  = 0x40;

// ---- Sync words ----
const uint8_t kSyncWordLoRaWan = 0x34;   // public LoRaWAN networks (TTN)
const uint8_t kSyncWordPrivate = 0x12;

} // namespace sx1276
