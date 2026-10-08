#include "DavisSchedule.h"

namespace DAVIS {

// The longest a plan runs before it is looked at again, even with nothing
// due: a cap, not a timing rule.
static const uint32_t kMaxPlanMs = 5000;
// A clock that goes back more than a second, or on more than ten minutes
// between two plans, was set: start over. (Ten minutes without a plan is
// past losing every station anyway.)
static const uint32_t kJumpBackMs = 1000;
static const uint32_t kJumpOnMs = 600000;

static inline int32_t diff(uint32_t a, uint32_t b) { return (int32_t)(a - b); }

void DavisSchedule::begin(davis_band_t band, uint8_t activeMask, uint32_t now, uint32_t ticksPerSecond, const Timing& timing) {
	bandId = band;
	nChannels = bandChannels(band);
	tps = ticksPerSecond ? ticksPerSecond : 16000;
	t = timing;
	for (uint8_t i = 0; i < kMaxStations; ++i) {
		st[i] = StationState();
		st[i].active = (activeMask >> i) & 1;
	}
	discTarget = -1;
	discChannel = 0;
	discStart = now;
	lastNow = now;
	haveLast = true;
}

uint8_t DavisSchedule::activeMask() const {
	uint8_t m = 0;
	for (uint8_t i = 0; i < kMaxStations; ++i) if (st[i].active) m |= (uint8_t)(1u << i);
	return m;
}

void DavisSchedule::setStationActive(uint8_t id, bool active, uint32_t now) {
	StationState& s = st[id & 7];
	if (s.active == active) return;
	s.active = active;
	s.synced = false;
	s.lostInARow = 0;
	if (!active && discTarget == (int8_t)(id & 7)) {
		discTarget = -1;
		discStart = now;
	}
}

void DavisSchedule::resync(uint32_t now) {
	for (uint8_t i = 0; i < kMaxStations; ++i) {
		st[i].synced = false;
		st[i].lostInARow = 0;
	}
	discTarget = -1;
	discStart = now;
	lastNow = now;
}

void DavisSchedule::shift(int32_t delta) {
	for (uint8_t i = 0; i < kMaxStations; ++i) {
		st[i].due += (uint32_t)delta;
		st[i].lastRx += (uint32_t)delta;
	}
	discStart += (uint32_t)delta;
	lastNow += (uint32_t)delta;
}

bool DavisSchedule::onPacket(uint8_t id, uint8_t channel, uint32_t rx) {
	StationState& s = st[id & 7];
	if (!s.active) return false;
	if (channel >= nChannels) channel = 0;
	s.synced = true;
	s.due = rx + interval(id);
	s.channel = next(channel);
	s.lostInARow = 0;
	s.lastRx = rx;
	++s.packets;
	if (discTarget == (int8_t)(id & 7)) {
		discTarget = -1;
		discStart = rx;
	}
	return true;
}

void DavisSchedule::lose(uint8_t id, uint32_t now) {
	StationState& s = st[id];
	s.synced = false;
	s.lostInARow = 0;
	++s.resyncs;
	// Look for it first where it was due next: if only the timing drifted,
	// it turns up there within a cycle.
	if (discTarget < 0) {
		discTarget = (int8_t)id;
		discChannel = s.channel;
		discStart = now;
	}
}

// The station to look for: the current one while it still needs looking
// for, else the next active unsynced one after it. advance: move on even
// if the current one still needs it (its time on this channel is up).
void DavisSchedule::pickDiscoveryTarget(uint32_t now, bool advance) {
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
	if (advance || from < 0) discStart = now;
}

bool DavisSchedule::expired(const Plan& p, uint32_t now) const {
	const int32_t left = diff(p.until, now);
	return left <= 0 || left > (int32_t)ticksFromMs(kMaxPlanMs + 1000);
}

DavisSchedule::Plan DavisSchedule::plan(uint32_t now) {
	if (haveLast) {
		const int32_t d = diff(now, lastNow);
		if (d < -(int32_t)ticksFromMs(kJumpBackMs) || d > (int32_t)ticksFromMs(kJumpOnMs)) {
			++jumps;
			resync(now);
		}
	}
	lastNow = now;
	haveLast = true;

	const int32_t guard = (int32_t)ticksFromMs(t.guardMs);
	const int32_t late = (int32_t)ticksFromMs(t.lateMs);
	const int32_t maxPlan = (int32_t)ticksFromMs(kMaxPlanMs);

	// Packets that were due and did not come.
	for (uint8_t i = 0; i < kMaxStations; ++i) {
		StationState& s = st[i];
		while (s.active && s.synced && diff(now, s.due + (uint32_t)late) > 0) {
			++s.missed;
			++s.lostInARow;
			s.due += interval(i);
			s.channel = next(s.channel);
			if (s.lostInARow >= t.resyncAfter) lose(i, now);
		}
	}

	// The synced station due soonest.
	int8_t best = -1;
	for (uint8_t i = 0; i < kMaxStations; ++i) {
		if (!st[i].active || !st[i].synced) continue;
		if (best < 0 || diff(st[i].due, st[best].due) < 0) best = (int8_t)i;
	}

	Plan p;
	p.discovery = false;
	p.station = -1;

	if (best >= 0 && diff(st[best].due - (uint32_t)guard, now) <= 0) {
		p.channel = st[best].channel;
		p.station = best;
		p.until = st[best].due + (uint32_t)late;
		return p;
	}

	pickDiscoveryTarget(now, false);
	if (discTarget >= 0) {
		// A cycle and one interval on each channel.
		const int32_t step = (int32_t)((nChannels + 1u) * interval((uint8_t)discTarget));
		if (diff(now, discStart) >= step) {
			discChannel = next(discChannel);
			pickDiscoveryTarget(now, true);
		}
		int32_t span = diff(discStart + (uint32_t)step, now);
		if (best >= 0) {
			const int32_t toBest = diff(st[best].due - (uint32_t)guard, now);
			if (toBest < span) span = toBest;
		}
		if (span > maxPlan) span = maxPlan;
		if (span < 1) span = 1;
		p.channel = discChannel;
		p.discovery = true;
		p.until = now + (uint32_t)span;
		return p;
	}

	if (best >= 0) {
		// Nothing to look for: wait on the next station's channel.
		p.channel = st[best].channel;
		p.station = best;
		int32_t span = diff(st[best].due + (uint32_t)late, now);
		if (span > maxPlan) span = maxPlan;
		p.until = now + (uint32_t)span;
		return p;
	}

	// No station active.
	p.channel = discChannel;
	p.until = now + (uint32_t)maxPlan;
	return p;
}

} /* namespace DAVIS */
