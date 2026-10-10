/*
 * iLoRaRadio.h
 *
 *  A LoRa radio as the LoRaWAN MAC (iRadio/lorawan) sees it: one request
 *  at a time, each with its own lora::Config, and events that carry the
 *  time they happened. rfm95<TTransport> is one; an SX126x driver would
 *  be another, with the MAC unchanged.
 *
 *  Every call is non-blocking. main() does the work and is called from
 *  one thread, the same one that makes the requests.
 */

#ifndef ILORARADIO_H_
#define ILORARADIO_H_

#include <stdint.h>
#include "LoRaPhy.h"

namespace lora {

enum EventKind : uint8_t {
	ev_tx_done,
	ev_rx_done,        // a packet, with its RSSI and SNR
	ev_rx_timeout,     // RX single: nothing within the symbols asked for
	ev_crc_error,      // a packet whose payload CRC failed (not delivered)
	ev_fault           // the radio stopped answering, or a TX never finished
};

struct Event {
	EventKind kind;
	uint32_t  ticks;     // when: the radio's time base (see iLoRaRadio::now())
	int16_t   rssiDbm;   // rx_done
	int8_t    snrDb;     // rx_done
	uint8_t   len;       // rx_done: bytes in the packet
};

class iLoRaRadio {
public:
	virtual ~iLoRaRadio() {}

	virtual void main(uint32_t nowMs) = 0;

	// False if busy or the request is invalid.
	virtual bool transmit(const Config& cfg, const uint8_t* data, uint8_t len) = 0;
	// timeoutSymbols 0: listen until standby() or the next request.
	virtual bool receive(const Config& cfg, uint16_t timeoutSymbols) = 0;
	virtual bool standby() = 0;
	virtual bool powerDown() = 0;
	// A request would be taken now.
	virtual bool accepting() const = 0;

	virtual bool takeEvent(Event* e, uint8_t* buf, uint8_t cap) = 0;

	// The time base of Event::ticks, read now (nowMs is what main() is
	// given; a radio with a clock ignores it).
	virtual uint32_t now(uint32_t nowMs) const = 0;
	virtual uint32_t ticksPerSecond() const = 0;
	// The highest power the radio can transmit, in dBm.
	virtual int8_t maxPowerDbm() const = 0;
	// Listening (RX continuous) and a packet is arriving: its header has
	// been heard and it hasn't finished. For listen-before-talk. A radio
	// that can't tell says false.
	virtual bool channelBusy() const { return false; }
	// Transfers are under way (starting up, configuring, reading a packet):
	// call main() again at once rather than sleeping.
	virtual bool busy() const { return false; }
};

} // namespace lora

#endif /* ILORARADIO_H_ */
