/*
 * rfm95.h
 *
 *  A LoRa radio: HopeRF RFM95W (Semtech SX1276), and its 433/868 MHz
 *  siblings (RFM96/97/98, SX1277/78/79), in LoRa mode. Non-blocking, on
 *  SensorStateMachine like the other drivers. rfm95<TTransport> inherits
 *  its transport, an ISensorTransport on SPI:
 *
 *      rfm95<Stm32HalSPITransport> radio(rfm95_default_param(), &hspi1, RFM_CS_GPIO_Port, RFM_CS_Pin, spi1Mutex);
 *
 *  The SX1276 sets bit 7 of the address to write; on an SPITransport the
 *  constructor switches it to SPITransport::AddressBit::WriteHigh.
 *
 *  This is the radio only: one request at a time, each with its own
 *  frequency, spreading factor, bandwidth, coding rate, preamble, CRC,
 *  I/Q inversion and power (lora::Config), which is how LoRaWAN uses a
 *  radio (a different channel and data rate for every uplink and each
 *  receive window). The LoRaWAN MAC goes on top of this, in iRadio/lorawan.
 *
 *      radio.transmit(cfg, data, len);       // then a TxDone event
 *      radio.receive(cfg, 8);                // one packet within 8 symbols
 *                                            //   (RX single): RxDone,
 *                                            //   CrcError or RxTimeout
 *      radio.receive(cfg, 0);                // RX continuous until standby()
 *      rfm95_event_t e; uint8_t buf[255];
 *      if (radio.takeEvent(&e, buf, sizeof buf)) ...
 *
 *  Each event carries the time it happened, in ticks of the clock given
 *  to setClock() (an iClock: Stm32RtcClock, say), or in ms of the nowMs
 *  passed to main() without one. With param.dio0Interrupt, onDio0()
 *  from DIO0's rising edge stamps TxDone and RxDone the moment they
 *  happen (LoRaWAN times its receive windows from the end of the
 *  uplink); without it, the flags are polled every param.pollMs and the
 *  time is when the poll saw them. DIO1 (RxTimeout) works the same way
 *  through onDio1().
 *
 *  Startup: RegVersion must read 0x12; then sleep, LoRa mode, standby,
 *  the FIFO bases, LNA gain and boost, AGC, the sync word, and a read
 *  back of the mode and the sync word. A failed transfer, a TX that never
 *  finishes, or a reset chip goes to the error state (an rfm95_ev_fault
 *  event, the request dropped), then starts again after kErrorBackoffMs.
 *
 *  Registers and values from Semtech's LoRaMac-node and arduino-LoRa
 *  (see SX1276Regs.h and LoRaPhy.h). Not run against a radio.
 */

#ifndef RFM95_H_
#define RFM95_H_

#include <stdint.h>
#include <string.h>
#include <type_traits>
#include <utility>
#include "ISensorTransport.h"
#include "SPITransport.h"
#include "SensorStateMachine.h"
#include "iClock.h"
#include "SX1276Regs.h"
#include "LoRaPhy.h"
#include "iLoRaRadio.h"

typedef enum rfm95_state_t {
	rfm95_init = 0,
	rfm95_read_version,
	rfm95_wait_version,
	rfm95_write_ops,            // the queued transfers (register writes, FIFO data), one each
	rfm95_wait_write_ops,
	rfm95_verify_mode,          // read back the op mode, then the sync word
	rfm95_wait_verify_mode,
	rfm95_verify_sync,
	rfm95_wait_verify_sync,
	rfm95_idle,                 // standby (or sleep), waiting for a request
	rfm95_tx_wait,              // transmitting: wait for TxDone
	rfm95_rx_wait,              // receiving: wait for RxDone or RxTimeout
	rfm95_read_status,          // RegFifoRxCurrentAddr..RegRxNbBytes (0x10..0x13)
	rfm95_wait_status,
	rfm95_read_signal,          // packet SNR and RSSI (0x19, 0x1A)
	rfm95_wait_signal,
	rfm95_read_fifo,            // the packet, 32 bytes at a time
	rfm95_wait_fifo,
	rfm95_error
} rfm95_state_t;

typedef struct rfm95_param_t {
	uint8_t syncWord;      // 0x34 public LoRaWAN (TTN), 0x12 private
	bool    dio0Interrupt; // onDio0() is called from DIO0's rising edge
	bool    dio1Interrupt; // onDio1() is called from DIO1's rising edge (RxTimeout)
	uint8_t pollMs;        // how often the IRQ flags are read while waiting (also a backstop with interrupts)
	int8_t  maxPowerDbm;   // transmit power is held at or below this (the module allows +20)
} rfm95_param_t;

inline rfm95_param_t rfm95_default_param() {
	rfm95_param_t p;
	p.syncWord = sx1276::kSyncWordLoRaWan;
	p.dio0Interrupt = false;
	p.dio1Interrupt = false;
	p.pollMs = 2;
	p.maxPowerDbm = 20;
	return p;
}

// The events are iLoRaRadio's (lora::Event); these are their names here.
// ticks is the clock's ticks, or ms without a clock.
typedef lora::EventKind rfm95_event_kind_t;
typedef lora::Event rfm95_event_t;
static constexpr lora::EventKind rfm95_ev_tx_done    = lora::ev_tx_done;
static constexpr lora::EventKind rfm95_ev_rx_done    = lora::ev_rx_done;
static constexpr lora::EventKind rfm95_ev_rx_timeout = lora::ev_rx_timeout;
static constexpr lora::EventKind rfm95_ev_crc_error  = lora::ev_crc_error;
static constexpr lora::EventKind rfm95_ev_fault      = lora::ev_fault;

typedef struct rfm95_stats_t {
	uint32_t txDone, rxDone, rxTimeouts, crcErrors;
	uint32_t eventsDropped;  // a packet arrived with both event slots full
	uint32_t faults;
} rfm95_stats_t;

template <typename TTransport>
class rfm95 : public SensorStateMachine<TTransport, rfm95_state_t>, public lora::iLoRaRadio {
	static_assert(std::is_base_of<ISensorTransport, TTransport>::value,
		"rfm95 needs an ISensorTransport (SPITransport, ...)");
	using Base = SensorStateMachine<TTransport, rfm95_state_t>;

public:
	static constexpr uint32_t kBusTimeoutMs = 100;
	static constexpr uint32_t kErrorBackoffMs = 1000;
	static constexpr uint32_t kTxMarginMs = 200;     // beyond the time on air, before a TX counts as stuck
	static constexpr uint32_t kRxMarginMs = 100;     // the same for an RX single (its symbol timeout and a 255 byte packet)
	static constexpr uint32_t kIdleSleepMs = 100;
	static constexpr uint8_t  kMaxPayload = 255;
	static constexpr uint8_t  kEvents = 2;

	template <typename... TArgs>
	explicit rfm95(const rfm95_param_t& param, TArgs&&... transportArgs);

	void main(uint32_t nowMs) override;

	// Requests. False if the radio is busy (still starting, transmitting,
	// in an RX single, in the error state, or a request is already
	// waiting) or the request is invalid. RX continuous takes a new request
	// (it ends the listening; a packet being read out is finished first).
	// The payload is copied. A receive with timeoutSymbols 0 runs until
	// standby() or another request.
	bool transmit(const lora::Config& cfg, const uint8_t* data, uint8_t len) override;
	bool receive(const lora::Config& cfg, uint16_t timeoutSymbols) override;
	// Ends RX continuous; the radio waits in standby. False if not receiving.
	bool standby() override;
	// Sleep (lowest power, the FIFO is lost) until the next request.
	bool powerDown() override;

	bool ready() const { return this->_state == rfm95_idle && _req == Req::None; }
	// ready(), or listening in RX continuous: a request will be taken.
	bool accepting() const override { return _req == Req::None && (this->_state == rfm95_idle || receivingContinuous()); }
	bool receivingContinuous() const { return _rxContinuous && this->_state >= rfm95_rx_wait && this->_state < rfm95_error; }

	// The next event, oldest first; the packet (rx_done) goes into buf, up
	// to cap bytes. False when there is none.
	bool takeEvent(rfm95_event_t* e, uint8_t* buf, uint8_t cap) override;

	// From DIO0's / DIO1's rising edge. Safe in an interrupt (iClock::now() is).
	void onDio0(uint32_t nowMs) { stampIrq(nowMs); }
	void onDio1(uint32_t nowMs) { stampIrq(nowMs); }

	// Time events by this clock instead of nowMs. Set before the first request.
	void setClock(iClock* clock) { _clock = clock; }
	uint32_t ticksPerSecond() const override { return _clock ? _clock->ticksPerSecond() : 1000u; }
	uint32_t now(uint32_t nowMs) const override { return nowTicks(nowMs); }
	int8_t maxPowerDbm() const override { return _param.maxPowerDbm < 20 ? _param.maxPowerDbm : 20; }

	const rfm95_stats_t& stats() const { return _st; }

protected:
	void onFail() override;
	// Called from onDio0()/onDio1(). An OS version wakes the thread.
	virtual void wake() {}
	bool irqPending() const { return _irqPending; }

private:
	struct Op { uint8_t reg; uint8_t len; uint8_t v[3]; const uint8_t* ptr; };
	enum class Req : uint8_t { None, Tx, Rx, Standby, Sleep };

	void opsClear() { _opCount = 0; _opPos = 0; }
	void op(uint8_t reg, uint8_t v) { op3(reg, 1, v, 0, 0); }
	void op3(uint8_t reg, uint8_t len, uint8_t v0, uint8_t v1, uint8_t v2);
	void opData(uint8_t reg, const uint8_t* p, uint8_t len);
	void runOps(rfm95_state_t after, uint32_t nowMs);
	void queueModem(const lora::Config& c, bool forTx);
	void startRequest(uint32_t nowMs);
	void stampIrq(uint32_t nowMs) {
		_irqTicks = _clock ? _clock->now() : nowMs;
		_irqPending = true;
		wake();
	}
	uint32_t nowTicks(uint32_t nowMs) const { return _clock ? _clock->now() : nowMs; }
	void push(rfm95_event_kind_t k, uint32_t ticks, int16_t rssi, int8_t snr, uint8_t len);
	bool waitFlags(uint32_t nowMs, uint32_t deadlineMs);

	rfm95_param_t _param;

	Op      _ops[32];
	uint8_t _opCount = 0, _opPos = 0;
	rfm95_state_t _afterOps = rfm95_idle;

	// The request.
	Req          _req = Req::None;
	lora::Config _cfg;
	uint16_t     _rxSymbols = 0;
	bool         _rxContinuous = false;
	uint8_t      _tx[kMaxPayload];
	uint8_t      _txLen = 0;
	uint32_t     _startMs = 0;        // when the TX or RX began, for the safety timeout
	uint32_t     _deadlineMs = 0;     // after that long, the TX/RX single is stuck (0: none)
	uint32_t     _lastPollMs = 0;

	// Reading a packet.
	uint8_t  _regs[4];                // 0x10..0x13, then 0x19..0x1A, version, read-backs
	uint8_t  _rxLen = 0, _rxPos = 0, _rxAddr = 0;
	uint8_t  _rxSnrRaw = 0, _rxRssiRaw = 0;
	uint32_t _rxTicks = 0;
	uint8_t  _rxBuf[kMaxPayload];
	uint8_t  _clearFlags[1] = {sx1276::kIrqAll};

	// Events.
	struct Slot { rfm95_event_t e; uint8_t data[kMaxPayload]; };
	Slot    _ev[kEvents];
	uint8_t _evHead = 0, _evCount = 0;

	volatile bool     _irqPending = false;
	volatile uint32_t _irqTicks = 0;
	iClock* _clock = nullptr;

	rfm95_stats_t _st = {};
};

#include "../src/rfm95.tpp"

#endif /* RFM95_H_ */
