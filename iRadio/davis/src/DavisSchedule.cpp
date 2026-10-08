#include "DavisSchedule.h"

namespace DAVIS {

// The maximum time a plan runs before it is looked at again, even with
// nothing due: a cap, not a timing rule.
static const uint32_t kMaxPlanMs = 5000;

static inline int32_t diff(uint32_t a, uint32_t b) { return (int32_t)(a - b); }

void DavisSchedule::begin(davis_band_t band, uint8_t activeMask, uint32_t nowMs, const Timing& timing) {
	bandId = band;
	nChannels = bandChannels(band);
	t = timing;
	for (uint8_t i = 0; i < kMaxStations; ++i) {
		st[i] = StationState();
		st[i].active = (activeMask >> i) & 1;
	}
	discTarget = -1;
	discChannel = 0;
	discStart16 = nowMs * 16u;
}

uint8_t DavisSchedule::activeMask() const {
	uint8_t m = 0;
	for (uint8_t i = 0; i < kMaxStations; ++i) if (st[i].active) m |= (uint8_t)(1u << i);
	return m;
}

void DavisSchedule::setStationActive(uint8_t id, bool active, uint32_t nowMs) {
	StationState& s = st[id & 7];
	if (s.active == active) return;
	s.active = active;
	s.synced = false;
	s.lostInARow = 0;
	if (!active && discTarget == (int8_t)(id & 7)) {
		discTarget = -1;
		discStart16 = nowMs * 16u;
	}
}

void DavisSchedule::resync(uint32_t nowMs) {
	for (uint8_t i = 0; i < kMaxStations; ++i) {
		st[i].synced = false;
		st[i].lostInARow = 0;
	}
	discTarget = -1;
	discStart16 = nowMs * 16u;
}

bool DavisSchedule::onPacket(uint8_t id, uint8_t channel, uint32_t rxMs) {
	StationState& s = st[id & 7];
	if (!s.active) return false;
	if (channel >= nChannels) channel = 0;
	s.synced = true;
	s.dueSixteenths = rxMs * 16u + intervalSixteenths(id);
	s.channel = next(channel);
	s.lostInARow = 0;
	s.lastRxMs = rxMs;
	++s.packets;
	if (discTarget == (int8_t)(id & 7)) {
		discTarget = -1;
		discStart16 = rxMs * 16u;
	}
	return true;
}

void DavisSchedule::lose(uint8_t id, uint32_t now16) {
	StationState& s = st[id];
	s.synced = false;
	s.lostInARow = 0;
	++s.resyncs;
	// Look for it first where it was due next: if only the timing drifted,
	// it turns up there within a cycle.
	if (discTarget < 0) {
		discTarget = (int8_t)id;
		discChannel = s.channel;
		discStart16 = now16;
	}
}

// The station to look for: the current one while it still needs looking
// for, else the next active unsynced one after it. advance: move on even
// if the current one still needs it (its time on this channel is up).
void DavisSchedule::pickDiscoveryTarget(uint32_t now16, bool advance) {
	if (!advance && discTarget >= 0 && st[discTarget].active && !st[discTarget].synced) return;
	const int8_t from = discTarget;
	discTarget = -1;
	for (uint8_t k = 1; k <= kMaxStations; ++k) {
		const uint8_t i = (uint8_t)((from + k + kMaxStations) % kMaxStations);
		if (st[i].active && !st[i].synced) {
			discTarget = (int8_t)i;
			break;
		}
	}
	if (advance || from < 0) discStart16 = now16;
}

uint32_t DavisSchedule::toMs(uint32_t nowMs, int32_t delta16) {
	if (delta16 <= 0) return nowMs;
	return nowMs + (uint32_t)((delta16 + 15) / 16);
}

DavisSchedule::Plan DavisSchedule::plan(uint32_t nowMs) {
	const uint32_t now16 = nowMs * 16u;
	const int32_t guard16 = (int32_t)t.guardMs * 16;
	const int32_t late16 = (int32_t)t.lateMs * 16;

	// Packets that were due and did not come.
	for (uint8_t i = 0; i < kMaxStations; ++i) {
		StationState& s = st[i];
		while (s.active && s.synced && diff(now16, s.dueSixteenths + (uint32_t)late16) > 0) {
			++s.missed;
			++s.lostInARow;
			s.dueSixteenths += intervalSixteenths(i);
			s.channel = next(s.channel);
			if (s.lostInARow >= t.resyncAfter) lose(i, now16);
		}
	}

	// The synced station due soonest.
	int8_t best = -1;
	for (uint8_t i = 0; i < kMaxStations; ++i) {
		if (!st[i].active || !st[i].synced) continue;
		if (best < 0 || diff(st[i].dueSixteenths, st[best].dueSixteenths) < 0) best = (int8_t)i;
	}

	Plan p;
	p.discovery = false;
	p.station = -1;

	if (best >= 0 && diff(st[best].dueSixteenths - (uint32_t)guard16, now16) <= 0) {
		p.channel = st[best].channel;
		p.station = best;
		p.untilMs = toMs(nowMs, diff(st[best].dueSixteenths + (uint32_t)late16, now16));
		return p;
	}

	pickDiscoveryTarget(now16, false);
	if (discTarget >= 0) {
		// A cycle and one interval on each channel.
		const int32_t step16 = (int32_t)((nChannels + 1u) * intervalSixteenths((uint8_t)discTarget));
		if (diff(now16, discStart16) >= step16) {
			discChannel = next(discChannel);
			pickDiscoveryTarget(now16, true);
		}
		int32_t until16 = diff(discStart16 + (uint32_t)step16, now16);
		if (best >= 0) {
			const int32_t toBest = diff(st[best].dueSixteenths - (uint32_t)guard16, now16);
			if (toBest < until16) until16 = toBest;
		}
		if (until16 > (int32_t)(kMaxPlanMs * 16)) until16 = (int32_t)(kMaxPlanMs * 16);
		p.channel = discChannel;
		p.discovery = true;
		p.untilMs = toMs(nowMs, until16);
		if (p.untilMs == nowMs) p.untilMs = nowMs + 1;
		return p;
	}

	if (best >= 0) {
		// Nothing to look for: wait on the next station's channel.
		p.channel = st[best].channel;
		p.station = best;
		int32_t until16 = diff(st[best].dueSixteenths + (uint32_t)late16, now16);
		if (until16 > (int32_t)(kMaxPlanMs * 16)) until16 = (int32_t)(kMaxPlanMs * 16);
		p.untilMs = toMs(nowMs, until16);
		return p;
	}

	// No station active.
	p.channel = discChannel;
	p.untilMs = nowMs + kMaxPlanMs;
	return p;
}

} /* namespace DAVIS */
