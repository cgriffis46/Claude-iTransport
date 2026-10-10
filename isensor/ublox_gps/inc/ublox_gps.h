/*
 * ublox_gps.h
 *
 *  A u-blox GNSS receiver on a UART: NMEA for the data, UBX to set it
 *  up. Non-blocking, on SensorStateMachine like the other drivers.
 *  ublox_gps<TTransport> inherits its transport, an iTransport:
 *
 *      ublox_gps_param_t param(ublox_config_t::ValSet);   // a u-blox 9 or 10
 *      param.measRateMs = 200;                             // 5 Hz
 *      ublox_gps<Stm32HalUartTransport> gps(param, &huart2);
 *
 *      for(;;){ gps.main(HAL_GetTick()); ... }
 *      if(gps.newData()){ GnssData d; gps.getData(&d); ... }
 *
 *  Setup: param.config says how to talk to the receiver.
 *   - None: listen only, to whatever the receiver sends at power up
 *     (u-blox: GGA, GLL, GSA, GSV, RMC, VTG at 1 Hz).
 *   - Legacy: u-blox 6, 7 and 8 (NEO-6M, NEO-M8N...). One UBX-CFG-MSG
 *     per sentence, then UBX-CFG-RATE.
 *   - ValSet: u-blox 9 and 10 (NEO-M9N, MAX-M10S...). One
 *     UBX-CFG-VALSET, to RAM only, so it is sent again at every start.
 *  Every message waits for its ACK (resent up to cfg_tries times). If
 *  the receiver answers NAK, or never answers (its RX not wired, a
 *  different baud rate), configStatus() says so and the driver carries
 *  on with what the receiver sends anyway: a fix at the receiver's
 *  defaults is worth more than none. The baud rate is not changed:
 *  configure the UART to the receiver's (9600 on u-blox 6 to 8, 38400
 *  on u-blox 10), and keep the sentences and rate within it (at 9600,
 *  GGA+RMC+GSA+GSV fit about 1 Hz).
 *
 *  The work is in two halves, as in PM25. onByteReceived() runs in the
 *  UART interrupt and only puts the byte in a ring buffer (ByteRing,
 *  in isensor/nmea with the parser). main() takes the bytes
 *  out, splits UBX frames from NMEA sentences, and parses them; the
 *  parsing is never done in the interrupt.
 *
 *  Readings: getData() gives everything NmeaParser has (time, date,
 *  position in 1e-7 degrees, altitude, speed, course, DOPs, satellites).
 *  Nothing is valid until the receiver has said so, and all of it goes
 *  back to "nothing known" when the receiver falls silent for
 *  silenceMs (the error state, which starts again after
 *  ublox_retry_ms, configuring again).
 *
 *  Not run against a receiver: the UBX numbers come from Zephyr's and
 *  SparkFun's u-blox code (see UbxProtocol.h).
 */

#ifndef UBLOX_GPS_H_
#define UBLOX_GPS_H_

#include <stdint.h>
#include <string.h>
#include <atomic>
#include <type_traits>
#include <utility>
#include "iTransport.h"
#include "SensorStateMachine.h"
#include "ByteRing.h"
#include "NmeaParser.h"
#include "UbxProtocol.h"

enum class ublox_config_t : uint8_t {
	None,     // listen only
	Legacy,   // UBX-CFG-MSG and UBX-CFG-RATE: u-blox 6, 7, 8
	ValSet    // UBX-CFG-VALSET: u-blox 9, 10
};

// One bit per ubx::Nmea sentence.
inline uint8_t ublox_sentence_bit(ubx::Nmea s) { return (uint8_t)(1u << (uint8_t)s); }

struct ublox_gps_param_t {
	ublox_config_t config;   // no default: which generation it is matters
	uint16_t measRateMs;     // ms between solutions: 1000 is 1 Hz, 100 is 10 Hz
	uint8_t  sentences;      // ublox_sentence_bit()s of the sentences wanted
	uint32_t silenceMs;      // nothing good for this long is a failure

	explicit ublox_gps_param_t(ublox_config_t c)
		: config(c), measRateMs(1000),
		  sentences((uint8_t)(ublox_sentence_bit(ubx::Nmea::GGA) | ublox_sentence_bit(ubx::Nmea::RMC) |
		                      ublox_sentence_bit(ubx::Nmea::GSA) | ublox_sentence_bit(ubx::Nmea::GSV))),
		  silenceMs(3000) {}
};

typedef enum ublox_gps_state_t {
	ublox_init_state,        // attach to the transport
	ublox_cfg_send_state,    // send the next configuration message
	ublox_cfg_wait_state,    // wait for its ACK
	ublox_listening_state,   // NMEA arriving
	ublox_error_state        // silent for silenceMs; data invalid; starts again
} ublox_gps_state_t;

enum class ublox_config_status_t : uint8_t {
	NotDone,    // not configured (yet, or config None)
	Done,       // every message acknowledged
	Rejected,   // the receiver answered NAK (wrong ublox_config_t for it?)
	NoAnswer    // no ACK after cfg_tries (receiver's RX not connected? baud?)
};

// All times in ms.
const uint32_t ublox_ack_timeout_ms = 500;    // per configuration message
const uint8_t  ublox_cfg_tries = 3;
const uint32_t ublox_write_timeout_ms = 200;  // the UART still busy with the last write
const uint32_t ublox_retry_ms = 1000;         // in the error state, before starting again
const uint32_t ublox_max_sleep_ms = 100;

template <typename TTransport>
class ublox_gps : public SensorStateMachine<TTransport, ublox_gps_state_t>, public iTransportRxSink {
	static_assert(std::is_base_of<iTransport, TTransport>::value,
			"ublox_gps<TTransport>: TTransport must derive from iTransport");
	typedef SensorStateMachine<TTransport, ublox_gps_state_t> base;
protected:
	using base::enter;
	using base::fail;
	using base::elapsed;
	using base::sleepRemaining;
	using base::sleep;
	using base::_state;
	using base::last_update;
public:
	static const uint16_t kRxRingSize = 512;   // power of two; 0.5 s at 9600 baud, 44 ms at 115200

	struct Stats {
		uint32_t rxOverflows;      // bytes lost because main() fell behind
		uint32_t ubxFrames, ubxErrors;
		uint32_t cfgResends;
	};

	template <typename... TArgs>
	explicit ublox_gps(const ublox_gps_param_t& param, TArgs&&... transportArgs);
	virtual ~ublox_gps() {}

	void main(uint32_t nowMs);

	// False until the receiver has sent a good GGA or RMC (and again
	// after it falls silent). Clears newData().
	bool getData(GnssData* out);
	// True when a GGA or RMC has arrived since the last getData().
	bool newData() const { return _newData; }
	// The receiver's own view: it has a fix. Latitude and longitude in
	// 1e-7 degrees. False (and nothing written) without one.
	bool position(int32_t* latE7, int32_t* lonE7) const;

	ublox_config_status_t configStatus() const { return _configStatus; }
	const NmeaParser::Stats& nmeaStats() const { return _nmea.stats(); }
	Stats stats() const;

	// Called by the transport for every byte received. Interrupt context.
	void onByteReceived(uint8_t byte) override;

protected:
	// Called from the interrupt at the end of each NMEA line, and every
	// 64 bytes. Empty here: main() finds the bytes the next time round.
	// An OS version wakes the thread that is in sleep().
	virtual void wake() {}
	// True once wake() has been called since main() last took the
	// bytes. For an OS version's sleep(), so it does not sleep through
	// bytes that are already waiting.
	bool wakePending() const { return _wakePending.load(std::memory_order_acquire); }

	void onFail() override;

private:
	void drain(uint32_t nowMs);
	void handleByte(uint8_t b, uint32_t nowMs);
	bool nextConfigMessage();   // builds the next one into _tx; false when there are none left

	ublox_gps_param_t _param;
	NmeaParser  _nmea;
	ubx::Parser _ubx;

	ByteRing<kRxRingSize> _rx;            // interrupt -> main()
	std::atomic<bool>     _wakePending;
	uint8_t               _sinceWake;     // interrupt only

	// Configuration.
	uint8_t  _tx[ubx::ValSet::kMaxPayload + 8];   // iTransport may send from it after write() returns
	size_t   _txLen;
	uint8_t  _cfgStep;
	uint8_t  _cfgTries;
	uint8_t  _waitCls, _waitId;
	enum class Ack : uint8_t { Waiting, Acked, Refused } _ack;
	ublox_config_status_t _configStatus;
	uint32_t _cfgResends;

	uint32_t _lastGoodMs;
	bool     _haveData;
	bool     _newData;
};

#include "../src/ublox_gps.tpp"

#endif /* UBLOX_GPS_H_ */
