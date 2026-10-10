#pragma once
// The arithmetic of the LoRa physical layer on an SX1276: register values
// for a frequency and a bandwidth, the low data rate rule, time on air,
// and the packet RSSI and SNR. Pure logic, header-only.
//
// The formulas follow Semtech's LoRaMac-node (sx1276.c): FRF = f * 2^19 /
// 32 MHz; LowDataRateOptimize at 125 kHz with SF11/12 and 250 kHz with
// SF12 (symbols over 16 ms); time on air from
// SX1276GetLoRaTimeOnAirNumerator; packet RSSI = -157 + raw + raw/16
// (+ SNR when it is negative) on the high frequency port.

#include <stdint.h>

namespace lora {

// Bandwidths as RegModemConfig1 codes.
enum class Bw : uint8_t {
	Bw7_8k = 0, Bw10_4k = 1, Bw15_6k = 2, Bw20_8k = 3, Bw31_25k = 4,
	Bw41_7k = 5, Bw62_5k = 6, Bw125k = 7, Bw250k = 8, Bw500k = 9
};

inline uint32_t bandwidthHz(Bw bw) {
	static const uint32_t hz[] = {7800, 10400, 15600, 20800, 31250, 41700, 62500, 125000, 250000, 500000};
	const uint8_t i = (uint8_t)bw;
	return i < 10 ? hz[i] : 125000;
}

// Everything that has to match between a transmitter and a receiver,
// plus the power. LoRaWAN changes these for every uplink and window.
struct Config {
	uint32_t freqHz;      // e.g. 902300000 (US915 channel 0)
	uint8_t  sf;          // spreading factor 7..12
	Bw       bw;
	uint8_t  cr;          // coding rate 4/(4+cr): 1..4, LoRaWAN uses 1 (4/5)
	uint16_t preamble;    // symbols, LoRaWAN 8
	bool     crc;         // payload CRC: on for uplinks, off for LoRaWAN downlinks
	bool     invertIq;    // LoRaWAN: off for uplinks, on for downlinks
	int8_t   powerDbm;    // transmit only, +2..+20 on the RFM95's PA_BOOST
};

inline Config defaultConfig(uint32_t freqHz) {
	Config c;
	c.freqHz = freqHz;
	c.sf = 7;
	c.bw = Bw::Bw125k;
	c.cr = 1;
	c.preamble = 8;
	c.crc = true;
	c.invertIq = false;
	c.powerDbm = 14;
	return c;
}

inline bool valid(const Config& c) {
	return c.sf >= 7 && c.sf <= 12 && (uint8_t)c.bw <= 9 && c.cr >= 1 && c.cr <= 4 &&
	       c.preamble >= 6 && c.freqHz >= 137000000u && c.freqHz <= 1020000000u;
}

// RegFrf for a frequency: f * 2^19 / 32 MHz, rounded down as Semtech's
// and arduino-LoRa's code do (one step is 61.035 Hz).
inline uint32_t frf(uint32_t freqHz) { return (uint32_t)(((uint64_t)freqHz << 19) / 32000000u); }
inline uint32_t freqFromFrf(uint32_t frfValue) { return (uint32_t)(((uint64_t)frfValue * 32000000u) >> 19); }

// One symbol, in microseconds: 2^SF / BW.
inline uint32_t symbolUs(uint8_t sf, Bw bw) {
	return (uint32_t)(((uint64_t)1000000u << sf) / bandwidthHz(bw));
}

// Symbols longer than 16 ms need LowDataRateOptimize: at 125 kHz SF11
// and SF12, at 250 kHz SF12 (Semtech's code names exactly these), and the
// same rule for the narrower bandwidths.
inline bool lowDataRateOptimize(uint8_t sf, Bw bw) { return symbolUs(sf, bw) > 16000u; }

// Time on air of a packet with an explicit header, in microseconds,
// rounded up: Semtech's SX1276GetLoRaTimeOnAirNumerator, in 1/4 symbols.
inline uint32_t timeOnAirUs(const Config& c, uint8_t payloadLen) {
	const int32_t sf = c.sf;
	const bool ldro = lowDataRateOptimize(c.sf, c.bw);
	int32_t num = (int32_t)payloadLen * 8 + (c.crc ? 16 : 0) - 4 * sf + 20;   // +20: explicit header
	int32_t den = 4 * sf;
	if (sf > 6) {
		num += 8;
		if (ldro) den = 4 * (sf - 2);
	}
	if (num < 0) num = 0;
	const int32_t symbols = ((num + den - 1) / den) * (c.cr + 4) + c.preamble + 12;
	const uint64_t quarterSymbolsTimes = (uint64_t)(4 * symbols + 1) << (sf - 2);   // = 2^SF * (symbols + 4.25)
	const uint64_t hz = bandwidthHz(c.bw);
	return (uint32_t)((quarterSymbolsTimes * 1000000u + hz - 1) / hz);
}

// Packet RSSI in dBm and SNR in dB (rounded), from RegPktRssiValue and
// RegPktSnrValue, on the high frequency port (above 525 MHz: 868/915).
inline int8_t packetSnrDb(uint8_t rawSnr) { return (int8_t)(((int8_t)rawSnr + 2) >> 2); }
inline int16_t packetRssiDbm(uint8_t rawRssi, uint8_t rawSnr, uint32_t freqHz) {
	const int16_t offset = freqHz > 525000000u ? -157 : -164;
	const int16_t r = rawRssi;
	const int8_t snr = packetSnrDb(rawSnr);
	return (int16_t)(offset + r + (r >> 4) + (snr < 0 ? snr : 0));
}

} // namespace lora
