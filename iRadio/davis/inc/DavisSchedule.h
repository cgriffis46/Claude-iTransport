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
 *  Time is counted in ticks of whatever clock the caller uses (its
 *  ticksPerSecond is given to begin()): the RTOS tick times 16 (16000 a
 *  second), or the STM32 RTC's subsecond counter (256 to 32768 a second;
 *  see Stm32RtcClock). A rate that is a multiple of 16 makes every
 *  interval a whole number of ticks, so a run of misses adds no rounding.
 *  Differences are taken as signed 32 bit, right across the wrap.
 *
 *  If the clock is set (an RTC set from SNTP), call shift() with how far
 *  it moved and the timing moves with it. A jump plan() sees for itself,
 *  more than a second back or ten minutes on, makes it start over; ask
 *  for plans when expired() says, which also catches a clock set back.
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
		uint32_t due = 0;			// when, in ticks (synced only)
		uint32_t lastRx = 0;
		uint16_t lostInARow = 0;
		uint32_t packets = 0;		// heard
		uint32_t missed = 0;		// due and not heard, while synced
		uint32_t resyncs = 0;		// times lost after being synced
	};

	// What to do now: listen on `channel` until `until` (ticks), then ask
	// again (or sooner, after a packet).
	struct Plan {
		uint8_t channel;
		uint32_t until;
		int8_t station;				// the synced station expected, or -1 while discovering or idle
		bool discovery;
	};

	// ticksPerSecond: of the clock now and every time below are counted
	// in. 16000 is the RTOS ms tick times 16.
	void begin(davis_band_t band, uint8_t activeMask, uint32_t now,
			   uint32_t ticksPerSecond = 16000, const Timing& timing = Timing());

	void setStationActive(uint8_t id, bool active, uint32_t now);
	// Forget every station's timing and look for them all again.
	void resync(uint32_t now);
	// The clock was set: everything moves by delta ticks, so no station
	// is lost over it.
	void shift(int32_t delta);

	// A packet with a good CRC from station id, heard on channel, ending
	// at rx. False when the station is not active (the packet is ignored,
	// and nothing changes).
	bool onPacket(uint8_t id, uint8_t channel, uint32_t rx);

	// Counts any packets missed up to now, then says where to listen.
	Plan plan(uint32_t now);

	// True when it is time to ask for a new plan: its end has come, or is
	// further off than any plan runs (the clock was set back).
	bool expired(const Plan& p, uint32_t now) const;

	const StationState& station(uint8_t id) const { return st[id & 7]; }
	davis_band_t band() const { return bandId; }
	uint8_t channels() const { return nChannels; }
	uint8_t discoveryChannel() const { return discChannel; }
	int8_t discoveryStation() const { return discTarget; }
	uint8_t activeMask() const;
	uint32_t ticksPerSecond() const { return tps; }
	uint32_t interval(uint8_t id) const { return intervalTicks(id, tps); }
	uint32_t ticksFromMs(uint32_t ms) const { return (uint32_t)(((uint64_t)ms * tps + 999u) / 1000u); }
	uint32_t clockJumps() const { return jumps; }

private:
	uint8_t next(uint8_t ch) const { return (uint8_t)((ch + 1) % nChannels); }
	void lose(uint8_t id, uint32_t now);
	void pickDiscoveryTarget(uint32_t now, bool advance);

	davis_band_t bandId = davis_band_us;
	uint8_t nChannels = 51;
	Timing t;
	StationState st[kMaxStations];
	int8_t discTarget = -1;			// the station being looked for
	uint8_t discChannel = 0;
	uint32_t discStart = 0;
	uint32_t tps = 16000;
	uint32_t lastNow = 0;
	bool haveLast = false;
	uint32_t jumps = 0;
};

} /* namespace DAVIS */

#endif /* DAVISSCHEDULE_H_ */
