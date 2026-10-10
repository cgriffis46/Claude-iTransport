/*
 * RegionUS915.h
 *
 *  US902-928 (RP002-1.0.x): 64 uplink channels of 125 kHz (902.3 MHz +
 *  0.2 MHz each, DR0-3), 8 of 500 kHz (903.0 MHz + 1.6 MHz each, DR4),
 *  8 downlink channels of 500 kHz (923.3 MHz + 0.6 MHz each, DR8-13).
 *  RX1 is on downlink channel (uplink channel mod 8); RX2 is 923.3 MHz at
 *  DR8. EIRP 30 dBm at TXPower 0, 2 dB less a step down to 14. No duty
 *  cycle; FCC 15.247 limits power to 26 dBm at DR4 and 21 dBm with fewer
 *  than 50 125 kHz channels enabled.
 *
 *  Most gateways (and The Things Network) listen on one sub-band of 8+1
 *  channels: subBand 2 is channels 8-15 and 65, TTN's. subBand 0 starts
 *  with all 72 channels enabled, as RP002 says, and then a join only
 *  reaches an 8-channel gateway one try in eight. The network narrows the
 *  plan with the join accept's CFList or LinkADRReq either way.
 *
 *  Joins alternate as LoRaMac-node's: eight 125 kHz tries at DR0, each in
 *  the next 8-channel group with a channel picked at random in it, then
 *  one 500 kHz try at DR4.
 *
 *  Tables and the LinkADRReq channel mask rules (ChMaskCntl 0-4: a 16
 *  channel block; 5: bits 0-7 each turn a block of eight 125 kHz channels
 *  and the matching 500 kHz channel on or off; 6: all 125 kHz on, ChMask
 *  for the 500 kHz ones; 7: all 125 kHz off, the same) from Semtech's
 *  LoRaMac-node v4.7.0 (RegionUS915.c, RegionBaseUS.c, RegionCommon.c).
 */

#ifndef REGIONUS915_H_
#define REGIONUS915_H_

#include "LoRaWanRegion.h"

namespace lorawan {

class RegionUS915 : public Region {
public:
	static const uint8_t kChannels = 72;

	// subBand 1-8 (TTN: 2), or 0 for all channels.
	explicit RegionUS915(uint8_t subBand = 2);

	void reset() override;
	uint8_t minTxDr() const override { return 0; }
	uint8_t maxTxDr() const override { return 4; }
	uint8_t maxTxPowerIndex() const override { return 14; }
	bool txDataRate(uint8_t dr, lora::Config* c) const override;
	bool rxDataRate(uint8_t dr, lora::Config* c) const override;
	uint8_t maxPayload(uint8_t dr) const override;
	bool nextChannel(uint8_t dr, bool joining, uint32_t rnd, uint8_t* channel, uint32_t* freqHz) override;
	uint8_t joinDataRate(uint16_t attempt) const override { return (attempt % 9 == 0) ? 4 : 0; }
	uint32_t rx1Frequency(uint8_t channel, uint32_t upFreqHz) const override;
	uint8_t rx1DataRate(uint8_t upDr, uint8_t rx1DrOffset) const override;
	uint32_t rx2DefaultFrequency() const override { return 923300000u; }
	uint8_t rx2DefaultDataRate() const override { return 8; }
	int8_t eirpDbm(uint8_t txPower, uint8_t dr) const override;
	uint8_t linkAdrReq(const uint8_t* cmds, uint8_t len, bool adrOn, AdrState* st, uint8_t* consumed) override;
	uint8_t rxParamSetupStatus(uint8_t rx1DrOffset, uint8_t rx2Dr, uint32_t freqHz) const override;
	void applyCfList(const uint8_t cfList[16]) override;
	uint8_t nextLowerDr(uint8_t dr) const override;
	void enableDefaultChannels() override;
	uint8_t stateSize() const override { return 10; }
	void saveState(uint8_t* p) const override;
	bool loadState(const uint8_t* p, uint8_t len) override;

	static uint32_t channelFrequency(uint8_t ch);
	bool enabled(uint8_t ch) const { return ch < kChannels && (_mask[ch >> 4] >> (ch & 15)) & 1; }
	uint8_t enabledCount125() const;

private:
	static uint8_t count(const uint16_t* m, uint8_t from, uint8_t to);
	bool anyCarries(const uint16_t* m, uint8_t dr) const;

	uint8_t  _subBand;
	uint16_t _default[5];
	uint16_t _mask[5];        // 0-3: channels 0-63, 4: channels 64-71 (low byte)
	uint16_t _remaining[5];   // not used since all were last used
	uint8_t  _joinGroup = 0;
};

} // namespace lorawan

#endif /* REGIONUS915_H_ */
