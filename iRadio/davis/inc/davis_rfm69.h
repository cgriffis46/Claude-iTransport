/*
 * davis_rfm69.h
 *
 *  Davis Vantage Pro2 / Vue ISS receiver on an RFM69 (SX1231),
 *  non-blocking state machine.
 *
 *  The bus is chosen by the template argument. davis_rfm69<TTransport>
 *  inherits from TTransport, which must be an ISensorTransport
 *  (itransport/inc/ISensorTransport.h):
 *
 *      davis_rfm69<Stm32HalSPITransport> radio(param, &hspi1, RFM_CS_GPIO_Port, RFM_CS_Pin, spi1Mutex);
 *
 *  Everything after param goes to the transport's own constructor. The
 *  RFM69 sets bit 7 of a register address to write, the opposite of
 *  most sensors; on an SPITransport the constructor switches it to
 *  SPITransport::AddressBit::WriteHigh. This file includes no HAL and no
 *  RTOS header.
 *
 *  Call main(now) every pass of the loop. It never waits on the bus:
 *  each register access is one state that issues it and one that waits
 *  for it. While it waits for a packet it calls sleep() with how long it
 *  may wait, an empty stub here; xdavis_rfm69 (hw/freertos) blocks on a
 *  task notification instead, which the DIO0 interrupt cuts short.
 *
 *  What it does: checks the chip's version register, writes the Davis
 *  settings (FSK 19.2 kb/s, 9.9 kHz deviation, sync word 0xCB89, fixed
 *  10 byte packets, no hardware CRC), reads two of them back, then
 *  follows DavisSchedule: tunes to the channel it names (standby, the
 *  frequency, clear the FIFO, receive) and waits for PayloadReady, from
 *  DIO0 (onDio0()) or by reading RegIrqFlags2 every pollMs. A packet's
 *  RSSI and frequency error are read, then the ten FIFO bytes; the bits
 *  are put back in order and the CRC checked. A good packet from an
 *  active station goes to the schedule (which retunes for the next one)
 *  and out through deliver(): into a small ring for readPacket() here,
 *  into a FreeRTOS queue in xdavis_rfm69.
 *
 *  Timing. With no clock given, packets are timed by the nowMs passed to
 *  main(), the RTOS tick: as good as the oscillator the CPU runs on.
 *  setClock() gives it a better one, an iClock such as Stm32RtcClock on
 *  the 32.768 kHz watch crystal, and then the schedule and every packet
 *  time are counted in that clock's ticks; nowMs is used only for bus
 *  timeouts. Packet arrival is stamped from the clock in onDio0(), or
 *  given exactly by onDio0At() from the RTC's timestamp unit. If the
 *  clock is set while running (from SNTP, say), the step shows up as the
 *  clock moving differently from nowMs; the schedule is shifted by it
 *  and no station is lost.
 *
 *  Repeated packets are ignored unless param.accept_repeater: a
 *  repeater's timing is not the station's, so they are not used for
 *  sync either way. The receiver never transmits.
 *
 *  Settings are those of DeKay's DavisRFM69, which have worked on the
 *  air for years. The state machine and the schedule are new and have
 *  not yet run against an RFM69 or an ISS.
 */

#ifndef DAVIS_RFM69_H_
#define DAVIS_RFM69_H_

#include <stdint.h>
#include <type_traits>
#include <utility>
#include "ISensorTransport.h"
#include "SPITransport.h"
#include "SensorStateMachine.h"
#include "iClock.h"
#include "RFM69Regs.h"
#include "DavisProtocol.h"
#include "DavisSchedule.h"

namespace DAVIS {

typedef enum davis_state_t{
	davis_init = 0,
	davis_read_version,
	davis_wait_version,
	davis_write_ops,				// the queued register writes, one transfer each
	davis_wait_write_ops,
	davis_verify,					// read the sync word back
	davis_wait_verify,
	davis_plan,						// ask the schedule; retune if it says another channel
	davis_listen,					// wait for PayloadReady or the end of the plan
	davis_read_flags,
	davis_wait_flags,
	davis_read_signal,				// FEI and RSSI
	davis_wait_signal,
	davis_read_fifo,
	davis_wait_fifo,
	davis_error
}davis_state_t;

typedef struct davis_param_t{
	davis_band_t band;
	uint8_t active_stations;		// bit n: listen for station id n (transmitter n + 1)
	int16_t rssi_threshold_dbm;		// the radio looks for a packet above this. -95 by default
	bool wide_bandwidth;			// 50 kHz instead of 25: for console retransmits
	bool dio0_interrupt;			// onDio0() is called from DIO0's rising edge
	uint8_t poll_ms;				// how often RegIrqFlags2 is read without DIO0; also the backstop with it
	bool accept_repeater;			// deliver repeated packets (never used for timing)
}davis_param_t;

inline davis_param_t davis_default_param() {
	davis_param_t p = {};
	p.band = davis_band_us;
	p.active_stations = 0x01;		// transmitter 1, as shipped
	p.rssi_threshold_dbm = -95;
	p.wide_bandwidth = false;
	p.dio0_interrupt = false;
	p.poll_ms = 2;
	p.accept_repeater = false;
	return p;
}

typedef struct davis_stats_t{
	uint32_t packets;				// delivered
	uint32_t crc_errors;
	uint32_t ignored;				// good CRC, but an inactive station or a repeater
	uint32_t dropped;				// delivered, but nowhere to put it
	uint32_t retunes;
	uint32_t clock_steps;			// times the clock was seen to be set, and the schedule moved
}davis_stats_t;

template <typename TTransport>
class davis_rfm69 : public SensorStateMachine<TTransport, davis_state_t> {
	static_assert(std::is_base_of<ISensorTransport, TTransport>::value,
		"davis_rfm69 needs an ISensorTransport (SPITransport, ...)");

	using Base = SensorStateMachine<TTransport, davis_state_t>;

public:
	static constexpr uint32_t kBusTimeoutMs = 100;
	static constexpr uint32_t kErrorBackoffMs = 1000;
	static constexpr uint8_t kRing = 4;				// packets kept for readPacket()
	static constexpr uint8_t kMaxOps = 24;

	template <typename... TArgs>
	explicit davis_rfm69(const davis_param_t& param, TArgs&&... transportArgs);

	void main(uint32_t nowMs);

	// Time packets by this clock instead of nowMs. Before the first
	// main(), or the schedule starts over (it is counted in the clock's
	// ticks). nullptr goes back to nowMs.
	void setClock(iClock* clock);
	uint32_t ticksPerSecond() const { return clock_ ? clock_->ticksPerSecond() : 16000u; }

	// From DIO0's rising edge (PayloadReady). Stamps the time from the
	// clock (or nowMs). Safe in an interrupt (iClock::now() is).
	void onDio0(uint32_t nowMs) {
		irq_ticks = clock_ ? clock_->now() : nowMs * 16u;
		irq_pending = true;
	}
	// The same with the time already known, in the clock's ticks: from the
	// RTC's timestamp unit (Stm32RtcClock::timestamp()), latched by DIO0
	// on the RTC_TS pin in hardware.
	void onDio0At(uint32_t ticks) { irq_ticks = ticks; irq_pending = true; }

	// The next packet received, oldest first. False when there is none.
	bool readPacket(DavisPacket& out);

	// Settings, applied at the next main().
	void setStationActive(uint8_t id, bool active);
	void setBand(davis_band_t band);
	void resync();

	const DavisSchedule& schedule() const { return sched; }
	const davis_stats_t& stats() const { return st; }
	bool ready() const { return this->_state >= davis_plan && this->_state < davis_error; }
	uint8_t tunedChannel() const { return tuned_channel; }
	const DavisSchedule::Plan& currentPlan() const { return plan_now; }

protected:
	// Where a good packet goes. Here: the ring for readPacket(), counting
	// a drop when it is full. xdavis_rfm69 overrides it.
	virtual void deliver(const DavisPacket& p);

	void onFail() override;

	davis_stats_t st = {};

private:
	struct Op { uint8_t reg; uint8_t len; uint8_t v[3]; };
	void opsClear() { op_count = 0; op_pos = 0; }
	void op(uint8_t reg, uint8_t v0) { op3(reg, 1, v0, 0, 0); }
	void op3(uint8_t reg, uint8_t len, uint8_t v0, uint8_t v1, uint8_t v2);
	void queueConfiguration();
	void queueTune(uint8_t channel);
	void runOps(davis_state_t after, uint32_t nowMs);
	void applyPending(uint32_t now);
	void handlePacket(uint32_t nowMs);
	uint32_t clockNow(uint32_t nowMs);
	uint32_t msFor(int32_t ticks) const;

	davis_param_t param;
	DavisSchedule sched;
	DavisSchedule::Plan plan_now = {0, 0, -1, false};

	Op ops[kMaxOps];
	uint8_t op_count = 0, op_pos = 0;
	davis_state_t after_ops = davis_plan;

	uint8_t reg_buf[4];				// version, flags, FEI/RSSI, sync word read back
	uint8_t fifo[kPacketLen];

	bool in_rx = false;
	uint8_t tuned_channel = 0xFF;
	uint32_t last_poll = 0;				// ms
	uint32_t rx_ticks = 0, rx_ms = 0;

	volatile bool irq_pending = false;
	volatile uint32_t irq_ticks = 0;

	iClock* clock_ = nullptr;
	bool clock_changed = false;
	bool have_last_time = false;
	uint32_t last_ms = 0, last_ticks = 0;

	// Settings asked for between main()s.
	bool band_pending = false, resync_pending = false;
	davis_band_t new_band = davis_band_us;
	uint8_t active_changes = 0;		// bit n: station n's active flag to apply
	uint8_t active_wanted = 0;
	bool schedule_started = false;

	DavisPacket ring[kRing];
	uint8_t ring_head = 0, ring_count = 0;
};

} /* namespace DAVIS */

#include "../src/davis_rfm69.tpp"

#endif /* DAVIS_RFM69_H_ */
