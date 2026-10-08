/*
 * DavisProtocol.h
 *
 *  What a Davis Vantage Pro2 / Vue ISS sends, as pure logic: the hop
 *  frequencies, the transmit interval, the bit order, the CRC, and what
 *  the bytes of a packet mean. No radio, no RTOS.
 *
 *  The protocol is Davis's, reverse engineered and documented by DeKay
 *  (github.com/dekay/DavisRFM69/wiki/Message-Protocol) and others. The
 *  hop tables are Davis's channel frequencies as RFM69 register values.
 *
 *  On the air: 4 preamble bytes 0xAA, the sync word 0xCB 0x89, then ten
 *  bytes sent least significant bit first:
 *
 *    0     message type (bits 7-4), battery low (bit 3), station id (bits 2-0)
 *    1     wind speed, mph
 *    2     wind direction
 *    3-5   the message's value (see decode())
 *    6-7   CRC-16-CCITT (0x1021, initial 0) of bytes 0-5, high byte first
 *    8-9   0xFF 0xFF, or repeater information. A repeated packet's CRC
 *          covers bytes 0-5 and 8-9.
 *
 *  Station ids are 0-7 here; the ISS's DIP switches and the console
 *  call them transmitter 1-8.
 *
 *  A station sends every (41 + id) / 16 seconds (2.5625 s for id 0) and
 *  moves to the next channel of its band's table each time.
 */

#ifndef DAVISPROTOCOL_H_
#define DAVISPROTOCOL_H_

#include <stdint.h>

namespace DAVIS {

static constexpr uint8_t kPacketLen = 10;
static constexpr uint8_t kMaxStations = 8;

typedef enum davis_band_t{
	davis_band_us = 0,		// 902-928 MHz, 51 channels
	davis_band_au,			// 918-926 MHz, 51 channels
	davis_band_eu,			// 868 MHz, 5 channels
	davis_band_nz			// 921-928 MHz, 51 channels
}davis_band_t;

uint8_t bandChannels(davis_band_t band);
// The three RegFrf bytes (MSB first) of a channel. channel < bandChannels().
const uint8_t* bandFrf(davis_band_t band, uint8_t channel);
// The channel's carrier frequency in Hz, for logs and tests.
uint32_t channelHz(davis_band_t band, uint8_t channel);

// A station's transmit interval in sixteenths of a millisecond, which
// holds it exactly: (41 + id) * 1000. The receiver counts time in these
// so 50 missed packets do not add up to 25 ms of rounding.
inline uint32_t intervalSixteenths(uint8_t id) { return (uint32_t)(41 + (id & 7)) * 1000u; }

uint8_t reverseBits(uint8_t b);

// CRC-16-CCITT as Davis uses it: polynomial 0x1021, starting from init.
uint16_t crc16(const uint8_t* buf, uint8_t len, uint16_t init = 0);

typedef enum davis_crc_t{
	davis_crc_bad = 0,
	davis_crc_direct,		// straight from the station
	davis_crc_repeater		// through a repeater: the CRC also covers bytes 8-9
}davis_crc_t;

// raw in normal bit order. An all-zero CRC is bad: an all-zero packet
// would otherwise pass.
davis_crc_t checkCrc(const uint8_t raw[kPacketLen]);

// The ten bytes a station would send for these six (normal bit order):
// the CRC in bytes 6-7 and 0xFF 0xFF after. For tests and simulators.
void buildPacket(const uint8_t data[6], uint8_t out[kPacketLen]);

// One packet as received.
struct DavisPacket {
	uint8_t raw[kPacketLen];	// normal bit order, CRC checked
	uint8_t station;			// 0-7
	uint8_t channel;			// index in the band's table
	int16_t rssi;				// dBm
	int32_t feiHz;				// frequency error the radio measured
	uint32_t rxMs;				// when it arrived (end of the packet)
	bool viaRepeater;
};

// Message types (byte 0, bits 7-4)
#define DAVIS_MSG_VCAP      0x2	// Vue: supercap voltage
#define DAVIS_MSG_UV        0x4
#define DAVIS_MSG_RAINSECS  0x5	// seconds between bucket tips
#define DAVIS_MSG_SOLAR     0x6
#define DAVIS_MSG_VSOLAR    0x7	// Vue: solar panel voltage
#define DAVIS_MSG_TEMP      0x8
#define DAVIS_MSG_GUST      0x9
#define DAVIS_MSG_HUMIDITY  0xA
#define DAVIS_MSG_RAIN      0xE	// bucket tip counter
#define DAVIS_MSG_SOILLEAF  0xF

// What one packet says. Every packet carries the wind; each also carries
// one other value, which `type` names.
struct DavisReading {
	uint8_t station;
	uint8_t type;				// DAVIS_MSG_*
	bool batteryLow;
	float windSpeedMph;
	float windDirDeg;			// NAN: no anemometer
	// The packet's own value, NAN when the sensor is missing:
	//   TEMP °F, HUMIDITY %RH, UV index, SOLAR W/m², VCAP/VSOLAR volts,
	//   GUST mph (gustIndex: which of the last packets had it),
	//   RAIN the bucket tip counter (0-127, wraps; 0.01" a tip),
	//   RAINSECS seconds between the last two tips (NAN: no rain now).
	// SOILLEAF and unknown types: NAN.
	float value;
	uint8_t gustIndex;
};

// vue: the station is a Vantage Vue, which sends wind direction with
// more resolution. False for a Vantage Pro2 ISS.
void decode(const uint8_t raw[kPacketLen], bool vue, DavisReading& r);

} /* namespace DAVIS */

#endif /* DAVISPROTOCOL_H_ */
