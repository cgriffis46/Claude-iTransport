/*
 * davis_rfm69.tpp
 *
 *  Member definitions of davis_rfm69<TTransport>. Included at the end of
 *  inc/davis_rfm69.h; not compiled on its own.
 */

#include "../inc/davis_rfm69.h"

namespace DAVIS {

namespace detail {
// An SPITransport is told the RFM69 sets bit 7 to write. Any other
// transport (a test's, say) takes plain register numbers already.
template <typename T> inline void rfm69AddressBit(T& t, std::true_type) {
	t.setAddressBit(SPITransport::AddressBit::WriteHigh);
}
template <typename T> inline void rfm69AddressBit(T&, std::false_type) {}

// With DIO0 wired, RegIrqFlags2 is still read this often, in case an
// edge was missed.
static const uint32_t kDio0BackstopMs = 100;
} // namespace detail

template <typename TTransport>
template <typename... TArgs>
davis_rfm69<TTransport>::davis_rfm69(const davis_param_t& p, TArgs&&... transportArgs)
	: Base(davis_init, davis_error, kBusTimeoutMs, std::forward<TArgs>(transportArgs)...),
	  param(p) {
	detail::rfm69AddressBit(static_cast<TTransport&>(*this), std::is_base_of<SPITransport, TTransport>());
	if (param.poll_ms == 0) param.poll_ms = 1;
	active_wanted = param.active_stations;
	new_band = param.band;
}

// ---- settings ----

template <typename TTransport>
void davis_rfm69<TTransport>::setStationActive(uint8_t id, bool active) {
	const uint8_t bit = (uint8_t)(1u << (id & 7));
	if (active) active_wanted |= bit; else active_wanted &= (uint8_t)~bit;
	active_changes |= bit;
}

template <typename TTransport>
void davis_rfm69<TTransport>::setBand(davis_band_t band) {
	new_band = band;
	band_pending = true;
}

template <typename TTransport>
void davis_rfm69<TTransport>::resync() {
	resync_pending = true;
}

template <typename TTransport>
void davis_rfm69<TTransport>::applyPending(uint32_t nowMs) {
	if (band_pending) {
		band_pending = false;
		param.band = new_band;
		sched.begin(new_band, active_wanted, nowMs);
		active_changes = 0;
		in_rx = false;						// retune: the table changed
	}
	if (active_changes) {
		for (uint8_t i = 0; i < kMaxStations; ++i)
			if (active_changes & (1u << i)) sched.setStationActive(i, (active_wanted >> i) & 1, nowMs);
		active_changes = 0;
	}
	if (resync_pending) {
		resync_pending = false;
		sched.resync(nowMs);
	}
}

// ---- packets out ----

template <typename TTransport>
void davis_rfm69<TTransport>::deliver(const DavisPacket& p) {
	if (ring_count >= kRing) {
		++st.dropped;
		return;
	}
	ring[(ring_head + ring_count) % kRing] = p;
	++ring_count;
}

template <typename TTransport>
bool davis_rfm69<TTransport>::readPacket(DavisPacket& out) {
	if (ring_count == 0) return false;
	out = ring[ring_head];
	ring_head = (uint8_t)((ring_head + 1) % kRing);
	--ring_count;
	return true;
}

template <typename TTransport>
void davis_rfm69<TTransport>::onFail() {
	// Whatever the chip was doing, it is configured and tuned again from
	// the start. The schedule keeps its timing: an outage shorter than
	// resyncAfter packets costs only the packets missed.
	in_rx = false;
	tuned_channel = 0xFF;
}

// ---- register writes ----

template <typename TTransport>
void davis_rfm69<TTransport>::op3(uint8_t reg, uint8_t len, uint8_t v0, uint8_t v1, uint8_t v2) {
	if (op_count >= kMaxOps) return;
	Op& o = ops[op_count++];
	o.reg = reg; o.len = len;
	o.v[0] = v0; o.v[1] = v1; o.v[2] = v2;
}

template <typename TTransport>
void davis_rfm69<TTransport>::runOps(davis_state_t after, uint32_t nowMs) {
	op_pos = 0;
	after_ops = after;
	this->enter(op_count ? davis_write_ops : after, nowMs);
}

// The Davis settings. Runs of neighbouring registers go in one transfer
// (the RFM69 moves to the next address after each byte).
template <typename TTransport>
void davis_rfm69<TTransport>::queueConfiguration() {
	int16_t thresh = param.rssi_threshold_dbm;
	if (thresh > 0) thresh = 0;
	if (thresh < -127) thresh = -127;

	opsClear();
	op(RFM69_REG_OPMODE, RFM69_OPMODE_STANDBY);
	op(RFM69_REG_DATAMODUL, 0x02);							// packet mode, FSK, Gaussian BT 0.5
	op3(RFM69_REG_BITRATEMSB, 2, 0x06, 0x83, 0);			// 32 MHz / 0x0683 = 19.2 kb/s
	op3(RFM69_REG_FDEVMSB, 2, 0x00, 0xA1, 0);				// 161 x 61 Hz = 9.8 kHz deviation
	op(RFM69_REG_AFCCTRL, 0x00);							// AFC low beta off
	op(RFM69_REG_LNA, 0x00);								// 50 ohm, gain set by the AGC
	op(RFM69_REG_RXBW, param.wide_bandwidth ? 0x4B : 0x4C);	// DCC 4 %, 50 or 25 kHz
	op(RFM69_REG_AFCBW, param.wide_bandwidth ? 0x4A : 0x4B);	// twice that during AFC
	op(RFM69_REG_AFCFEI, 0x0C);							// AFC at each RX start, cleared each time
	op3(RFM69_REG_DIOMAPPING1, 2, RFM69_DIO0_PAYLOADREADY, RFM69_CLKOUT_OFF, 0);
	op(RFM69_REG_RSSITHRESH, (uint8_t)(-thresh * 2));
	op3(RFM69_REG_PREAMBLEMSB, 2, 0x00, 0x04, 0);			// Davis sends 4 bytes of preamble
	op(RFM69_REG_SYNCCONFIG, 0x8A);							// sync on, 2 bytes, 2 bit errors allowed
	op3(RFM69_REG_SYNCVALUE1, 2, 0xCB, 0x89, 0);
	op(RFM69_REG_PACKETCONFIG1, 0x08);						// fixed length, no CRC check (the bits are reversed)
	op(RFM69_REG_PAYLOADLENGTH, kPacketLen);
	op(RFM69_REG_PACKETCONFIG2, 0x12);						// restart RX 2 bit times after a packet is read
	op(RFM69_REG_TESTDAGC, 0x30);							// improved fading margin, low modulation index
	op(RFM69_REG_TESTAFC, 0x00);
	op(RFM69_REG_IRQFLAGS2, RFM69_IRQ2_FIFOOVERRUN);		// clears the FIFO
}

template <typename TTransport>
void davis_rfm69<TTransport>::queueTune(uint8_t channel) {
	const uint8_t* f = bandFrf(param.band, channel);
	opsClear();
	op(RFM69_REG_OPMODE, RFM69_OPMODE_STANDBY);
	op3(RFM69_REG_FRFMSB, 3, f[0], f[1], f[2]);			// takes effect as the LSB is written
	op(RFM69_REG_IRQFLAGS2, RFM69_IRQ2_FIFOOVERRUN);		// anything left from the last channel
	op(RFM69_REG_OPMODE, RFM69_OPMODE_RX);
}

// ---- a packet ----

template <typename TTransport>
void davis_rfm69<TTransport>::handlePacket(uint32_t nowMs) {
	DavisPacket p;
	for (uint8_t i = 0; i < kPacketLen; ++i) p.raw[i] = reverseBits(fifo[i]);	// sent LSB first
	const int16_t fei = (int16_t)((reg_buf[0] << 8) | reg_buf[1]);
	p.feiHz = (int32_t)fei * 15625 / 256;					// x 61.035 Hz
	p.rssi = (int16_t)(-(int16_t)reg_buf[3] / 2);
	p.channel = tuned_channel;
	p.rxMs = rx_ms;
	p.station = p.raw[0] & 0x07;
	p.viaRepeater = false;

	// Reading the FIFO empty has restarted RX on this channel already
	// (AutoRxRestart): a bad or ignored packet just goes back to listening.
	switch (checkCrc(p.raw)) {
	case davis_crc_bad:
		++st.crc_errors;
		this->enter(davis_listen, nowMs);
		return;
	case davis_crc_repeater:
		p.viaRepeater = true;
		if (param.accept_repeater && sched.station(p.station).active) {
			++st.packets;
			deliver(p);
		} else {
			++st.ignored;
		}
		this->enter(davis_listen, nowMs);
		return;
	case davis_crc_direct:
	default:
		break;
	}

	if (sched.onPacket(p.station, p.channel, p.rxMs)) {
		++st.packets;
		deliver(p);
		this->enter(davis_plan, nowMs);			// retune for what comes next
	} else {
		++st.ignored;
		this->enter(davis_listen, nowMs);
	}
}

// ---- the state machine ----

template <typename TTransport>
void davis_rfm69<TTransport>::main(uint32_t nowMs) {
	switch (this->_state) {
	case davis_init:
		in_rx = false;
		tuned_channel = 0xFF;
		this->enter(davis_read_version, nowMs);
		break;

	case davis_read_version:
		this->issued(this->readRegs(RFM69_REG_VERSION, reg_buf, 1), davis_wait_version, nowMs);
		break;

	case davis_wait_version:
		if (this->landed(nowMs)) {
			if (reg_buf[0] != RFM69_VERSION) {
				this->fail(nowMs);					// no RFM69 there, or the bus is wired wrong
				break;
			}
			queueConfiguration();
			runOps(davis_verify, nowMs);
		}
		break;

	case davis_write_ops: {
		const Op& o = ops[op_pos];
		this->issued(this->writeRegs(o.reg, o.v, o.len), davis_wait_write_ops, nowMs);
		break;
	}

	case davis_wait_write_ops:
		if (this->landed(nowMs)) {
			if (++op_pos < op_count) this->enter(davis_write_ops, nowMs);
			else this->enter(after_ops, nowMs);
		}
		break;

	case davis_verify:
		// A write that went out as a read (the address bit the wrong way
		// round) leaves the sync word as it was: catch that here.
		this->issued(this->readRegs(RFM69_REG_SYNCVALUE1, reg_buf, 2), davis_wait_verify, nowMs);
		break;

	case davis_wait_verify:
		if (this->landed(nowMs)) {
			if (reg_buf[0] != 0xCB || reg_buf[1] != 0x89) {
				this->fail(nowMs);
				break;
			}
			if (!schedule_started) {
				sched.begin(param.band, active_wanted, nowMs);
				active_changes = 0;
				schedule_started = true;
			}
			this->enter(davis_plan, nowMs);
		}
		break;

	case davis_plan:
		applyPending(nowMs);
		plan_now = sched.plan(nowMs);
		last_poll = nowMs;
		if (!in_rx || plan_now.channel != tuned_channel) {
			queueTune(plan_now.channel);
			tuned_channel = plan_now.channel;
			in_rx = true;
			irq_pending = false;					// an edge from the last channel
			++st.retunes;
			runOps(davis_listen, nowMs);
		} else {
			this->enter(davis_listen, nowMs);
		}
		break;

	case davis_listen: {
		if (band_pending || active_changes || resync_pending) {
			this->enter(davis_plan, nowMs);
			break;
		}
		const uint32_t every = param.dio0_interrupt ? detail::kDio0BackstopMs : param.poll_ms;
		const int32_t toEnd = (int32_t)(plan_now.untilMs - nowMs);
		const int32_t toPoll = (int32_t)(every - (nowMs - last_poll));
		if (irq_pending || toPoll <= 0 || toEnd <= 0) {
			this->enter(davis_read_flags, nowMs);
		} else {
			// Nothing to do until the next poll or the end of the plan;
			// xdavis_rfm69 wakes early on DIO0.
			this->sleep((uint32_t)(toEnd < toPoll ? toEnd : toPoll));
		}
		break;
	}

	case davis_read_flags:
		this->issued(this->readRegs(RFM69_REG_IRQFLAGS2, reg_buf, 1), davis_wait_flags, nowMs);
		break;

	case davis_wait_flags:
		if (this->landed(nowMs)) {
			last_poll = nowMs;
			const bool fromIrq = irq_pending;
			irq_pending = false;
			if (reg_buf[0] & RFM69_IRQ2_PAYLOADREADY) {
				rx_ms = fromIrq ? irq_ms : nowMs;
				this->enter(davis_read_signal, nowMs);
			} else if ((int32_t)(plan_now.untilMs - nowMs) <= 0) {
				this->enter(davis_plan, nowMs);
			} else {
				this->enter(davis_listen, nowMs);
			}
		}
		break;

	case davis_read_signal:
		// RegFeiMsb, RegFeiLsb, RegRssiConfig, RegRssiValue.
		this->issued(this->readRegs(RFM69_REG_FEIMSB, reg_buf, 4), davis_wait_signal, nowMs);
		break;

	case davis_wait_signal:
		if (this->landed(nowMs)) this->enter(davis_read_fifo, nowMs);
		break;

	case davis_read_fifo:
		// RegFifo again and again gives the next byte each time.
		this->issued(this->readRegs(RFM69_REG_FIFO, fifo, kPacketLen), davis_wait_fifo, nowMs);
		break;

	case davis_wait_fifo:
		if (this->landed(nowMs)) handlePacket(nowMs);
		break;

	case davis_error:
		if (this->errorCleared(nowMs, kErrorBackoffMs)) this->enter(davis_init, nowMs);
		break;

	default:
		this->enter(davis_init, nowMs);
		break;
	}
}

} /* namespace DAVIS */
