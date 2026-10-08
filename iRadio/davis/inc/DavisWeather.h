/*
 * DavisWeather.h
 *
 *  One station's weather, kept up to date from its packets. Each packet
 *  carries the wind and one other reading; this holds the latest of
 *  each, and adds up the rain from the bucket tip counter (which counts
 *  0-127 and wraps). Pure logic.
 *
 *      DavisWeather iss(false);                 // a Vantage Pro2 ISS
 *      DavisPacket p;
 *      while (xQueueReceive(radio.packets(), &p, portMAX_DELAY) == pdPASS)
 *          if (p.station == 0) iss.update(p);
 *      float t = iss.temperatureF;              // NAN until the first temperature packet
 */

#ifndef DAVISWEATHER_H_
#define DAVISWEATHER_H_

#include <stdint.h>
#include "DavisProtocol.h"

namespace DAVIS {

class DavisWeather {
public:
	explicit DavisWeather(bool vue = false);

	// Folds in one packet. Packets from other stations are the caller's
	// to keep out.
	void update(const DavisPacket& p);
	void update(const DavisReading& r, uint32_t nowMs);

	bool vue;

	float temperatureF;
	float humidity;				// %RH
	float windSpeedMph;
	float windDirDeg;
	float windGustMph;			// the 10 minute gust the station reports
	float uvIndex;
	float solarWm2;
	float rainRateInHr;			// from the time between the last two tips; 0 when it is not raining
	uint32_t rainTips;			// since this object started (0.01" each)
	float superCapV, solarPanelV;	// Vue only
	bool batteryLow;
	uint32_t updates;
	uint32_t lastUpdateMs;

	float rainInches() const { return (float)rainTips * 0.01f; }

private:
	bool haveRainCounter;
	uint8_t lastRainCounter;
};

} /* namespace DAVIS */

#endif /* DAVISWEATHER_H_ */
