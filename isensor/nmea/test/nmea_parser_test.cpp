// Host test for NmeaParser.
//
// g++ -std=gnu++14 -fno-exceptions -fno-rtti -Wall -Wextra -I../inc nmea_parser_test.cpp -o nmea_parser_test
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>
#include "NmeaParser.h"

static int g_checks = 0, g_failures = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { ++g_failures; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
static bool near(float a, float b, float tol) { return fabsf(a - b) <= tol; }

// "$" + body + "*hh\r\n", the checksum worked out here.
static std::string sentence(const std::string& body) {
	uint8_t x = 0;
	for (char c : body) x ^= (uint8_t)c;
	char tail[8];
	snprintf(tail, sizeof tail, "*%02X\r\n", x);
	return "$" + body + tail;
}

// Feeds a whole string; returns the last non-None result.
static NmeaParser::Sentence feedAll(NmeaParser& p, const std::string& s) {
	NmeaParser::Sentence last = NmeaParser::Sentence::None;
	for (char c : s) {
		NmeaParser::Sentence r = p.feed((uint8_t)c);
		if (r != NmeaParser::Sentence::None) last = r;
	}
	return last;
}

// The two sentences every NMEA description quotes, with their published
// checksums: these check the checksum against something not written here.
static void testClassicSentences() {
	NmeaParser p;
	CHECK(feedAll(p, "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47\r\n") == NmeaParser::Sentence::GGA);
	const GnssData& d = p.data();
	CHECK(d.timeValid && d.hour == 12 && d.minute == 35 && d.second == 19 && d.millisecond == 0);
	CHECK(d.positionValid && d.fixQuality == 1 && d.satellitesUsed == 8);
	CHECK(d.latitudeE7 == 481173000);       // 48 deg 07.038 min = 48.1173 deg
	CHECK(d.longitudeE7 == 115166667);      // 11 deg 31.000 min = 11.5166667 deg
	CHECK(near(d.hdop, 0.9f, 1e-4f) && near(d.altitudeM, 545.4f, 1e-3f) && near(d.geoidSeparationM, 46.9f, 1e-4f));

	CHECK(feedAll(p, "$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6A\r\n") == NmeaParser::Sentence::RMC);
	CHECK(d.dateValid && d.day == 23 && d.month == 3 && d.year == 1994);
	CHECK(near(d.speedMps, 22.4f * 0.514444f, 1e-3f) && near(d.courseDeg, 84.4f, 1e-4f));
	CHECK(d.modeIndicator == 0);            // NMEA 2.0: no mode field
	CHECK(p.stats().sentences == 2 && p.stats().checksumErrors == 0 && p.stats().malformed == 0);
}

static void testChecksumAndFraming() {
	NmeaParser p;
	// One character changed: the checksum no longer matches.
	CHECK(feedAll(p, "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.5,M,46.9,M,,*47\r\n") == NmeaParser::Sentence::Bad);
	CHECK(p.stats().checksumErrors == 1);
	CHECK(!p.data().positionValid && isnan(p.data().altitudeM));   // nothing taken over

	// Lower-case hex is accepted.
	CHECK(feedAll(p, "$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6a\r\n") == NmeaParser::Sentence::RMC);

	// The line ends before a checksum.
	CHECK(feedAll(p, "$GPGGA,123519,4807.038,N\r\n") == NmeaParser::Sentence::Bad);
	CHECK(p.stats().malformed == 1);

	// A "$" part way: the first is cut short, the second still counts.
	CHECK(feedAll(p, "$GPGGA,1235" + sentence("GPZDA,082710.00,16,09,2002,00,00")) == NmeaParser::Sentence::ZDA);
	CHECK(p.stats().malformed == 2);
	CHECK(p.data().year == 2002 && p.data().month == 9 && p.data().day == 16);

	// Longer than 82 characters.
	CHECK(feedAll(p, sentence("GPTXT," + std::string(90, 'A'))) == NmeaParser::Sentence::Bad);
	CHECK(p.stats().malformed == 3);

	// Binary in the middle (a UBX frame): rejected, and the next one is fine.
	std::string mixed = "$GPGGA,12";
	mixed += (char)0xB5; mixed += (char)0x62;
	mixed += sentence("GPZDA,082711.00,17,09,2002,00,00");
	CHECK(feedAll(p, mixed) == NmeaParser::Sentence::ZDA && p.data().day == 17);

	// Text between sentences is ignored.
	CHECK(feedAll(p, "noise\r\n") == NmeaParser::Sentence::None);

	// Proprietary and other sentences are good but not used.
	CHECK(feedAll(p, sentence("PUBX,00,081350.00,4717.113210,N")) == NmeaParser::Sentence::Other);
	CHECK(feedAll(p, sentence("GPTXT,01,01,02,u-blox ag - www.u-blox.com")) == NmeaParser::Sentence::Other);
}

// u-blox with no fix yet: empty fields everywhere.
static void testNoFix() {
	NmeaParser p;
	CHECK(feedAll(p, sentence("GPGGA,,,,,,0,00,99.99,,,,,,")) == NmeaParser::Sentence::GGA);
	const GnssData& d = p.data();
	CHECK(!d.timeValid && !d.positionValid && d.fixQuality == 0);
	CHECK(near(d.hdop, 99.99f, 1e-3f) && isnan(d.altitudeM));
	CHECK(feedAll(p, sentence("GPRMC,,V,,,,,,,,,,N")) == NmeaParser::Sentence::RMC);
	CHECK(!d.positionValid && !d.dateValid && isnan(d.speedMps) && d.modeIndicator == 'N');
	CHECK(feedAll(p, sentence("GPGSA,A,1,,,,,,,,,,,,,99.99,99.99,99.99")) == NmeaParser::Sentence::GSA);
	CHECK(d.fixType == 1);
	// Time known, no fix.
	CHECK(feedAll(p, sentence("GPGGA,092725.00,,,,,0,00,99.99,,,,,,")) == NmeaParser::Sentence::GGA);
	CHECK(d.timeValid && d.hour == 9 && d.minute == 27 && d.second == 25 && !d.positionValid);
}

// u-blox's own examples, multi-GNSS, NMEA 4.10 fields, southern and
// western hemispheres, and the high precision mode's 7 places.
static void testModernSentences() {
	NmeaParser p;
	const GnssData& d = p.data();
	CHECK(feedAll(p, sentence("GNRMC,083559.00,A,4717.11437,N,00833.91522,E,0.004,77.52,091202,,,A,V")) == NmeaParser::Sentence::RMC);
	CHECK(d.positionValid && d.modeIndicator == 'A' && d.millisecond == 0);
	CHECK(d.latitudeE7 == 472852395);      // 47 + 17.11437/60 = 47.28523950
	CHECK(d.longitudeE7 == 85652537);      // 8 + 33.91522/60 = 8.565253667 -> 85652537
	CHECK(d.year == 2002 && d.month == 12 && d.day == 9);

	CHECK(feedAll(p, sentence("GNGGA,092725.50,3352.1234567,S,15112.7654321,W,2,12,0.80,12.3,M,-25.1,M,1.0,0000")) == NmeaParser::Sentence::GGA);
	CHECK(d.millisecond == 500 && d.fixQuality == 2 && d.satellitesUsed == 12);
	CHECK(d.latitudeE7 == -338687243);     // -(33 + 52.1234567/60) = -33.8687243
	CHECK(d.longitudeE7 == -1512127572);   // -(151 + 12.7654321/60)
	CHECK(near(d.geoidSeparationM, -25.1f, 1e-4f));

	CHECK(feedAll(p, sentence("GNGSA,A,3,80,71,73,79,69,,,,,,,,1.83,1.09,1.47,1")) == NmeaParser::Sentence::GSA);
	CHECK(d.fixType == 3 && near(d.pdop, 1.83f, 1e-4f) && near(d.hdop, 1.09f, 1e-4f) && near(d.vdop, 1.47f, 1e-4f));

	CHECK(feedAll(p, sentence("GPVTG,77.52,T,,M,0.004,N,0.008,K,A")) == NmeaParser::Sentence::VTG);
	CHECK(near(d.speedMps, 0.008f / 3.6f, 1e-5f) && near(d.courseDeg, 77.52f, 1e-4f));

	CHECK(feedAll(p, sentence("GPGLL,4717.11364,N,00833.91565,E,092321.00,A,A")) == NmeaParser::Sentence::GLL);
	CHECK(d.positionValid && d.hour == 9 && d.minute == 23 && d.second == 21);

	CHECK(feedAll(p, sentence("GPZDA,082710.00,16,09,2002,00,00")) == NmeaParser::Sentence::ZDA);
	CHECK(d.dateValid && d.year == 2002);
}

static void testSatellites() {
	NmeaParser p;
	const GnssData& d = p.data();
	// GPS, three messages, the last one short, NMEA 4.10 signal ID 1.
	CHECK(feedAll(p, sentence("GPGSV,3,1,09,09,,,17,10,,,40,12,,,49,13,,,35,1")) == NmeaParser::Sentence::GSV);
	CHECK(feedAll(p, sentence("GPGSV,3,2,09,15,,,44,17,,,45,19,,,44,24,,,50,1")) == NmeaParser::Sentence::GSV);
	CHECK(feedAll(p, sentence("GPGSV,3,3,09,25,,,40,1")) == NmeaParser::Sentence::GSV);
	CHECK(d.satelliteCount == 9);
	CHECK(d.satellites[0].system == 'P' && d.satellites[0].prn == 9 && d.satellites[0].snr == 17);
	CHECK(d.satellites[0].elevation == -128 && d.satellites[0].azimuth == -1 && d.satellites[0].signalId == 1);
	// GLONASS, older format (no signal ID), one not tracked.
	CHECK(feedAll(p, sentence("GLGSV,1,1,02,65,45,123,38,66,10,300,")) == NmeaParser::Sentence::GSV);
	CHECK(d.satelliteCount == 11);
	CHECK(d.satellites[9].system == 'L' && d.satellites[9].elevation == 45 && d.satellites[9].azimuth == 123);
	CHECK(d.satellites[10].snr == -1);
	// GPS starts again: its nine are replaced, GLONASS's two stay.
	CHECK(feedAll(p, sentence("GPGSV,1,1,02,09,50,100,30,10,20,200,31,1")) == NmeaParser::Sentence::GSV);
	CHECK(d.satelliteCount == 4);
	int gps = 0, glo = 0;
	for (int i = 0; i < d.satelliteCount; ++i) { gps += d.satellites[i].system == 'P'; glo += d.satellites[i].system == 'L'; }
	CHECK(gps == 2 && glo == 2);
	// A different signal of GPS (ID 7, L5) does not replace L1's.
	CHECK(feedAll(p, sentence("GPGSV,1,1,01,09,50,100,28,7")) == NmeaParser::Sentence::GSV);
	CHECK(d.satelliteCount == 5);
	// More than fit: the rest are dropped, nothing overruns.
	for (int m = 1; m <= 12; ++m) {
		char b[96];
		snprintf(b, sizeof b, "GAGSV,12,%d,48,%d,10,10,30,%d,10,10,30,%d,10,10,30,%d,10,10,30,7",
		         m, m * 4, m * 4 + 1, m * 4 + 2, m * 4 + 3);
		feedAll(p, sentence(b));
	}
	CHECK(d.satelliteCount == GnssData::kMaxSatellites);
	// Malformed: message number beyond the total.
	CHECK(feedAll(p, sentence("GPGSV,1,2,01,09,50,100,28")) == NmeaParser::Sentence::Bad);
	// A malformed satellite (elevation 95) in a first message: rejected,
	// and the list is not cleared.
	const uint8_t before = d.satelliteCount;
	CHECK(feedAll(p, sentence("GPGSV,1,1,02,09,50,100,28,10,95,100,28,1")) == NmeaParser::Sentence::Bad);
	CHECK(d.satelliteCount == before);
	// Five satellites in one message is not NMEA.
	CHECK(feedAll(p, sentence("GPGSV,1,1,05,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5")) == NmeaParser::Sentence::Bad);
}

static void testFieldParsers() {
	int32_t e7;
	CHECK(NmeaParser::parseCoordinate("0000.00000", 10, &e7) && e7 == 0);
	CHECK(NmeaParser::parseCoordinate("18000.00000", 11, &e7) && e7 == 1800000000);
	CHECK(NmeaParser::parseCoordinate("4807", 4, &e7) && e7 == 481166667);   // no fraction
	CHECK(!NmeaParser::parseCoordinate("4860.000", 8, &e7));                  // 60 minutes
	CHECK(!NmeaParser::parseCoordinate("18100.000", 9, &e7));                 // over 180 degrees
	CHECK(!NmeaParser::parseCoordinate("48A7.038", 8, &e7));
	CHECK(!NmeaParser::parseCoordinate("7.038", 5, &e7));
	float f;
	CHECK(NmeaParser::parseFloat("-12.5", 5, &f) && f == -12.5f);
	CHECK(NmeaParser::parseFloat(".5", 2, &f) && f == 0.5f);
	CHECK(!NmeaParser::parseFloat("", 0, &f) && isnan(f));
	CHECK(!NmeaParser::parseFloat("1.2.3", 5, &f));
	CHECK(!NmeaParser::parseFloat("-", 1, &f));
	uint32_t u;
	CHECK(NmeaParser::parseUint("042", 3, &u) && u == 42);
	CHECK(!NmeaParser::parseUint("4x", 2, &u) && !NmeaParser::parseUint("", 0, &u));

	NmeaParser p;
	// Malformed fields make the whole sentence Bad, and change nothing.
	CHECK(feedAll(p, sentence("GPGGA,123519,4807.038,X,01131.000,E,1,08,0.9,545.4,M,46.9,M,,")) == NmeaParser::Sentence::Bad);
	CHECK(feedAll(p, sentence("GPGGA,256119,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,")) == NmeaParser::Sentence::Bad);
	CHECK(feedAll(p, sentence("GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,321394,003.1,W")) == NmeaParser::Sentence::Bad);
	CHECK(feedAll(p, sentence("GPGGA,123519")) == NmeaParser::Sentence::Bad);   // too few fields
	CHECK(!p.data().timeValid && p.stats().sentences == 0 && p.stats().malformed == 4);

	// invalidate(): back to nothing known.
	feedAll(p, "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47\r\n");
	CHECK(p.data().positionValid);
	p.invalidate();
	CHECK(!p.data().positionValid && !p.data().timeValid && isnan(p.data().hdop) && p.data().satelliteCount == 0);
}

int main() {
	testClassicSentences();
	testChecksumAndFraming();
	testNoFix();
	testModernSentences();
	testSatellites();
	testFieldParsers();
	printf("nmea_parser_test: %d checks, %d failures\n", g_checks, g_failures);
	return g_failures ? 1 : 0;
}
