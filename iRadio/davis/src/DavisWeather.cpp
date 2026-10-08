#include "DavisWeather.h"

namespace DAVIS {

static inline float nan_() { return __builtin_nanf(""); }
static inline bool isnan_(float v) { return v != v; }

DavisWeather::DavisWeather(bool isVue)
	: vue(isVue), temperatureF(nan_()), humidity(nan_()), windSpeedMph(nan_()), windDirDeg(nan_()),
	  windGustMph(nan_()), uvIndex(nan_()), solarWm2(nan_()), rainRateInHr(nan_()), rainTips(0),
	  superCapV(nan_()), solarPanelV(nan_()), batteryLow(false), updates(0), lastUpdateMs(0),
	  haveRainCounter(false), lastRainCounter(0) {}

void DavisWeather::update(const DavisPacket& p) {
	DavisReading r;
	decode(p.raw, vue, r);
	update(r, p.rxMs);
}

void DavisWeather::update(const DavisReading& r, uint32_t nowMs) {
	++updates;
	lastUpdateMs = nowMs;
	batteryLow = r.batteryLow;
	windSpeedMph = r.windSpeedMph;
	windDirDeg = r.windDirDeg;

	switch (r.type) {
	case DAVIS_MSG_TEMP:     temperatureF = r.value; break;
	case DAVIS_MSG_HUMIDITY: humidity = r.value; break;
	case DAVIS_MSG_UV:       uvIndex = r.value; break;
	case DAVIS_MSG_SOLAR:    solarWm2 = r.value; break;
	case DAVIS_MSG_GUST:     windGustMph = r.value; break;
	case DAVIS_MSG_VCAP:     superCapV = r.value; break;
	case DAVIS_MSG_VSOLAR:   solarPanelV = r.value; break;
	case DAVIS_MSG_RAIN:
		if (!isnan_(r.value)) {
			const uint8_t c = (uint8_t)r.value;
			// The first counter seen is where counting starts; after that,
			// the tips since the last one, across the wrap at 128.
			if (haveRainCounter) rainTips += (uint8_t)((c - lastRainCounter) & 0x7F);
			lastRainCounter = c;
			haveRainCounter = true;
		}
		break;
	case DAVIS_MSG_RAINSECS:
		// One 0.01" tip in that many seconds; no tip for a long time: 0.
		rainRateInHr = isnan_(r.value) || r.value <= 0.0f ? 0.0f : 36.0f / r.value;
		break;
	default:
		break;
	}
}

} /* namespace DAVIS */
