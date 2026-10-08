/*
 * DavisSchedule.h
 *
 *  Which channel to listen on, and until when, to hear up to eight Davis
 *  stations with one receiver. Pure logic: give it the time and the
 *  packets that arrive; it says where to tune. No radio, no RTOS.
 *
 *  A station hops to the next channel of its band every time it sends,
 *  every (41 + id) / 16 s. Once one of its packets has been heard, the
 *  next is known: one interval later, one channel on. So:
 *
 *    synced      tune to the station's next channel guardMs before it is
 *                due and listen until lateMs after. A packet re-anchors
 *                the timing; a packet not heard by then is missed, and
 *                the station is expected one interval later, one channel
 *                further on. resyncAfter misses in a row and it is lost.
 *    discovery   while a station is not synced, sit on one channel. The
 *                station comes round to every channel once a cycle
 *                (channels x interval, 131 s for id 0 in the US), so it
 *                is heard within one cycle unless that channel is jammed
 *                or the receiver was away serving a synced station just
 *                then. After a cycle and an interval more, move on to the
 *                next channel. A station lost after being synced is
 *                looked for first on the channel it was due on next.
 *
 *  Several stations: a synced station due soon always wins; discovery
 *  uses the time in between. Two synced stations due at the same moment
 *  on different channels cannot both be heard; the one due first is.
 *
 *  Time is counted in sixteenths of a millisecond, in which the
 *  intervals are whole numbers, and differences are taken as signed 32
 *  bit, so it is right across the rollover of a 1 ms tick.
 */

#ifndef DAVISSCHEDULE_H_
#define DAVISSCHEDULE_H_

#include <stdint.h>
#include "DavisProtocol.h"

namespace DAVIS {

struct DavisTiming {
	// Tune in this long before a packet is due. A packet is on the air
	// 6.7 ms before it is due (when it ends), and the radio must be
	// settled on the channel before it starts, after noticing the plan
	// has ended, retuning (four transfers) and any wait for a bus shared
	// with other chips. In the host test, where a transfer takes about a
	// millisecond, packets are lost below 18 ms; real SPI transfers take
	// microseconds, so 30 leaves a wide margin.
	uint16_t guardMs = 30;
	uint16_t lateMs = 20;			// and give up on it this long after
	uint16_t resyncAfter = 50;		// misses in a row before a station is lost
};

class DavisSchedule {
public:
	typedef DavisTiming Timing;

	struct StationState {
		bool active = false;		// listened for
		bool synced = false;
		uint8_t channel = 0;		// where the next packet is due (or, unsynced, was last due)
		uint32_t dueSixteenths = 0;	// when (synced only)
		uint32_t lastRxMs = 0;
		uint16_t lostInARow = 0;
		uint32_t packets = 0;		// heard
		uint32_t missed = 0;		// due and not heard, while synced
		uint32_t resyncs = 0;		// times lost after being synced
	};

	// What to do now: listen on `channel` until `untilMs`, then ask again
	// (or sooner, after a packet).
	struct Plan {
		uint8_t channel;
		uint32_t untilMs;
		int8_t station;				// the synced station expected, or -1 while discovering or idle
		bool discovery;
	};

	void begin(davis_band_t band, uint8_t activeMask, uint32_t nowMs, const Timing& timing = Timing());

	void setStationActive(uint8_t id, bool active, uint32_t nowMs);
	// Forget every station's timing and look for them all again.
	void resync(uint32_t nowMs);

	// A packet with a good CRC from station id, heard on channel, ending
	// at rxMs. False when the station is not active (the packet is
	// ignored, and nothing changes).
	bool onPacket(uint8_t id, uint8_t channel, uint32_t rxMs);

	// Counts any packets missed up to now, then says where to listen.
	Plan plan(uint32_t nowMs);

	const StationState& station(uint8_t id) const { return st[id & 7]; }
	davis_band_t band() const { return bandId; }
	uint8_t channels() const { return nChannels; }
	uint8_t discoveryChannel() const { return discChannel; }
	int8_t discoveryStation() const { return discTarget; }
	uint8_t activeMask() const;

private:
	uint8_t next(uint8_t ch) const { return (uint8_t)((ch + 1) % nChannels); }
	void lose(uint8_t id, uint32_t now16);
	void pickDiscoveryTarget(uint32_t now16, bool advance);
	static uint32_t toMs(uint32_t nowMs, int32_t delta16);

	davis_band_t bandId = davis_band_us;
	uint8_t nChannels = 51;
	Timing t;
	StationState st[kMaxStations];
	int8_t discTarget = -1;			// the station being looked for
	uint8_t discChannel = 0;
	uint32_t discStart16 = 0;
};

} /* namespace DAVIS */

#endif /* DAVISSCHEDULE_H_ */
