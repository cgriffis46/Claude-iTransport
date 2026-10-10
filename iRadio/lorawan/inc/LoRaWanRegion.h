/*
 * LoRaWanRegion.h
 *
 *  What differs between LoRaWAN regions (RP002 Regional Parameters): the
 *  channels and their data rates, the receive windows, transmit power,
 *  payload limits, and how LinkADRReq, RXParamSetupReq and the join
 *  accept's CFList change the channel plan. The MAC (LoRaWanMac) asks the
 *  region and stays the same for every region. RegionUS915 is the one
 *  written so far.
 *
 *  Data rates are LoRaWAN's DR numbers. Transmit power is the TXPower
 *  index: 0 is the region's highest EIRP, each step 2 dB less.
 */

#ifndef LORAWANREGION_H_
#define LORAWANREGION_H_

#include <stdint.h>
#include "LoRaPhy.h"

namespace lorawan {

// What LinkADRReq can change.
struct AdrState {
	uint8_t dr;        // uplink data rate
	uint8_t txPower;   // TXPower index
	uint8_t nbTrans;   // transmissions of each uplink (1..15)
};

class Region {
public:
	virtual ~Region() {}

	// The default channel plan (what a join starts from).
	virtual void reset() = 0;

	virtual uint8_t minTxDr() const = 0;
	virtual uint8_t maxTxDr() const = 0;
	virtual uint8_t defaultTxPower() const { return 0; }
	virtual uint8_t maxTxPowerIndex() const = 0;     // the lowest power allowed

	// Spreading factor and bandwidth for an uplink / a downlink data rate
	// (frequency, power and the rest are left as they are). False if the
	// region has no such data rate.
	virtual bool txDataRate(uint8_t dr, lora::Config* c) const = 0;
	virtual bool rxDataRate(uint8_t dr, lora::Config* c) const = 0;

	// Most bytes of FOpts + FRMPayload at a data rate (RP002's N).
	virtual uint8_t maxPayload(uint8_t dr) const = 0;

	// The channel for the next uplink at this data rate, from those enabled
	// and not used since all were last used. rnd is a fresh random number.
	// False if no enabled channel carries the data rate.
	virtual bool nextChannel(uint8_t dr, bool joining, uint32_t rnd, uint8_t* channel, uint32_t* freqHz) = 0;

	// The data rate for join attempt n (1, 2, 3...).
	virtual uint8_t joinDataRate(uint16_t attempt) const = 0;

	// RX1: frequency for an uplink on this channel, and its data rate.
	virtual uint32_t rx1Frequency(uint8_t channel, uint32_t upFreqHz) const = 0;
	virtual uint8_t rx1DataRate(uint8_t upDr, uint8_t rx1DrOffset) const = 0;
	virtual uint32_t rx2DefaultFrequency() const = 0;
	virtual uint8_t rx2DefaultDataRate() const = 0;

	// The EIRP for a TXPower index at this data rate (the region's limits
	// applied), in dBm.
	virtual int8_t eirpDbm(uint8_t txPower, uint8_t dr) const = 0;

	// A block of LinkADRReq commands, starting at cmds[0] (the CID), len
	// bytes to the end of the commands. Every LinkADRReq in a row is one
	// block, applied all or nothing. Returns the status for LinkADRAns
	// (bit 0 channel mask, 1 data rate, 2 power: 1 = accepted) and how many
	// bytes the block took; on 0x07 the channel plan and *st are changed.
	// With ADR off only the channel mask is taken.
	virtual uint8_t linkAdrReq(const uint8_t* cmds, uint8_t len, bool adrOn, AdrState* st, uint8_t* consumed) = 0;

	// RXParamSetupReq: status bits (0 frequency, 1 RX2 data rate, 2 RX1
	// offset; 1 = accepted). Changes nothing; the MAC applies it on 0x07.
	virtual uint8_t rxParamSetupStatus(uint8_t rx1DrOffset, uint8_t rx2Dr, uint32_t freqHz) const = 0;

	// NewChannelReq / DlChannelReq: false where the region doesn't have
	// them (the command is then skipped without an answer, as LoRaMac-node does).
	virtual bool supportsNewChannel() const { return false; }

	// The join accept's CFList.
	virtual void applyCfList(const uint8_t cfList[16]) = 0;

	// ADR backoff: the next lower data rate any enabled channel carries,
	// and all the default channels back on.
	virtual uint8_t nextLowerDr(uint8_t dr) const = 0;
	virtual void enableDefaultChannels() = 0;

	// The channel plan, kept with the session.
	virtual uint8_t stateSize() const = 0;
	virtual void saveState(uint8_t* p) const = 0;
	virtual bool loadState(const uint8_t* p, uint8_t len) = 0;
};

} // namespace lorawan

#endif /* LORAWANREGION_H_ */
