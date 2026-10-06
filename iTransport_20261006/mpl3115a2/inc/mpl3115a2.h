/*
 * mpl3115a2.h
 *
 *  Created on: Dec 29, 2025
 *      Author: coryg
 *
 *  MPL3115A2 pressure / altitude / temperature sensor, non-blocking
 *  state machine.
 *
 *  The bus is chosen by the template argument. mpl3115a2<TTransport>
 *  inherits from TTransport, which must be an ISensorTransport
 *  (itransport/inc/ISensorTransport.h):
 *
 *      mpl3115a2::mpl3115a2<Stm32HalI2CTransport>  sensor(param, &hi2c1, mpl3115a2::MPL3115A2_DEV_ADDRESS, i2c1Mutex);
 *
 *  Everything after param goes to the transport's own constructor.
 *  This file includes no HAL and no RTOS header.
 *
 *  Call main(now) every pass of the loop. It never waits on the bus:
 *  each register access is one state that issues the transfer and one
 *  state that waits for it to land. When a state has nothing to do it
 *  calls sleep(), an empty stub here. Override sleep() for an OS (see
 *  xmpl3115a2.h) to give up the CPU for that long.
 *
 *  The state bookkeeping, sleep() and the issue/wait helpers are
 *  inherited from SensorStateMachine (isensor/inc), shared with the
 *  other drivers.
 */

#ifndef MPL3115A2_H_
#define MPL3115A2_H_

#include <stdint.h>
#include <math.h>
#include <type_traits>
#include <utility>
#include "ISensorTransport.h"
#include "SensorStateMachine.h"

namespace mpl3115a2 {

#define mpl3115a2_who_am_i 0xC4
const uint16_t MPL3115A2_DEV_ADDRESS = 0x60;

typedef enum mpl3115a2_reg_addr_t{
	mpl3115a2_status_addr = 0x00,
	mpl3115a2_out_p_msb_addr = 0x01,
	mpl3115a2_out_p_csb_addr = 0x02,
	mpl3115a2_out_p_lsb_addr = 0x03,
	mpl3115a2_out_t_msb_addr = 0x04,
	mpl3115a2_out_t_lsb_addr = 0x05,
	mpl3115a2_dr_status_addr = 0x06,
	mpl3115a2_out_p_delta_msb_addr = 0x07,
	mpl3115a2_out_p_delta_csb_addr = 0x08,
	mpl3115a2_out_p_delta_lsb_addr = 0x09,
	mpl3115a2_out_t_delta_msb_addr = 0x0A,
	mpl3115a2_out_t_delta_lsb_addr = 0x0B,
	mpl3115a2_who_am_i_addr = 0x0C,
	mpl3115a2_f_status_addr = 0x0D,
	mpl3115a2_f_data_addr = 0x0E,
	mpl3115a2_f_setup = 0x0F,
	mpl3115a2_time_dly_addr = 0x10,
	mpl3115a2_sysmod_addr = 0x11,
	mpl3115a2_int_source_addr = 0x12,
	mpl3115a2_pt_data_cfg_addr = 0x13,
	mpl3115a2_bar_in_msb_addr = 0x14,
	mpl3115a2_bar_in_lsb_addr = 0x15,
	mpl3115a2_p_tgt_msb_addr = 0x16,
	mpl3115a2_p_tgt_lsb_addr = 0x17,
	mpl3115a2_t_tgt_addr = 0x18,
	mpl3115a2_p_wnd_msb_addr = 0x19,
	mpl3115a2_p_wnd_lsb_addr = 0x1A,
	mpl3115a2_t_wnd_addr = 0x1B,
	mpl3115a2_p_min_msb_addr = 0x1C,
	mpl3115a2_p_min_csb_addr = 0x1D,
	mpl3115a2_p_min_lsb_addr = 0x1E,
	mpl3115a2_t_min_msb_addr = 0x1F,
	mpl3115a2_t_min_lsb_addr = 0x20,
	mpl3115a2_p_max_msb_addr = 0x21,
	mpl3115a2_p_max_csb_addr = 0x22,
	mpl3115a2_p_max_lsb_addr = 0x23,
	mpl3115a2_t_max_msb_addr = 0x24,
	mpl3115a2_t_max_lsb_addr = 0x25,
	mpl3115a2_ctrl_reg1_addr = 0x26,
	mpl3115a2_ctrl_reg2_addr = 0x27,
	mpl3115a2_ctrl_reg3_addr = 0x28,
	mpl3115a2_ctrl_reg4_addr = 0x29,
	mpl3115a2_ctrl_reg5_addr = 0x2A,
	mpl3115a2_off_p_addr = 0x2B,
	mpl3115a2_off_t_addr = 0x2C,
	mpl3115a2_off_h_addr = 0x2D
}mpl3115a2_reg_addr_t;

// Every bus access is two states: one that issues the transfer and a
// wait_ state that it lands in.
typedef enum mpl3115a2_state_t{
	mpl3115a2_init_state,						// pause, then check a device answers
	mpl3115a2_whoami_state,						// issue the who_am_i read
	mpl3115a2_wait_whoami_state,
	mpl3115a2_reset_state,						// issue the software reset
	mpl3115a2_wait_reset_state,
	mpl3115a2_reset_settle_state,				// pause between RST polls
	mpl3115a2_read_reset_state,					// issue the ctrl_reg1 read: has RST cleared
	mpl3115a2_wait_read_reset_state,
	mpl3115a2_initializing_state,				// issue the ctrl_reg1 write
	mpl3115a2_wait_ctrl_reg1_state,
	mpl3115a2_pt_data_cfg_state,
	mpl3115a2_wait_pt_data_cfg_state,
	mpl3115a2_ctrl_reg5_state,					// interrupt routing, after SetIRQ()
	mpl3115a2_wait_ctrl_reg5_state,
	mpl3115a2_ctrl_reg4_state,					// interrupt enables, after SetIRQ()
	mpl3115a2_wait_ctrl_reg4_state,
	mpl3115a2_done_state,						// between measurements
	mpl3115a2_initiate_one_shot_measurement,	// issue the ctrl_reg1 read: is one already running
	mpl3115a2_wait_ost_state,
	mpl3115a2_write_ost_state,					// issue the ctrl_reg1 write with OST set
	mpl3115a2_wait_write_ost_state,
	mpl3115a2_wait_one_shot_measurement,		// pause between status polls
	mpl3115a2_status_state,						// issue the status read
	mpl3115a2_wait_status_state,
	mpl3115a2_conversion_state,					// issue the 5 byte data read
	mpl3115a2_wait_conversion_state,
	mpl3115a2_error_state						// pause, then start again from mpl3115a2_init_state
}mpl3115a2_state_t;

typedef enum mpl3115a2_mode_t{
	mpl3115a2_altimeter_mode=0b1,
	mpl3115a2_barometer_mode=0b0
}mpl3115a2_mode_t;

/** MPL3115A2 oversample values **/
enum {
  MPL3115A2_CTRL_REG1_OS1 = 0b000,
  MPL3115A2_CTRL_REG1_OS2 = 0b001,
  MPL3115A2_CTRL_REG1_OS4 = 0b010,
  MPL3115A2_CTRL_REG1_OS8 = 0b011,
  MPL3115A2_CTRL_REG1_OS16 = 0b100,
  MPL3115A2_CTRL_REG1_OS32 = 0b101,
  MPL3115A2_CTRL_REG1_OS64 = 0b110,
  MPL3115A2_CTRL_REG1_OS128 = 0b111,
};

typedef union {
	struct{
		uint8_t INT_EN_TCHG : 1;
		uint8_t INT_EN_PCHG : 1;
		uint8_t INT_EN_TTH : 1;
		uint8_t INT_EN_PTH : 1;
		uint8_t INT_EN_TW : 1;
		uint8_t INT_EN_PW : 1;
		uint8_t INT_EN_FIFO : 1;
		uint8_t INT_EN_DRDY : 1;
	}bit;
	uint8_t reg;
}irq_en_param_t;

typedef union {
	struct{
		uint8_t INT_CFG_TCHG : 1;
		uint8_t INT_CFG_PCHG : 1;
		uint8_t INT_CFG_TTH : 1;
		uint8_t INT_CFG_PTH : 1;
		uint8_t INT_CFG_TW : 1;
		uint8_t INT_CFG_PW : 1;
		uint8_t INT_CFG_FIFO : 1;
		uint8_t INT_CFG_DRDY : 1;
	}bit;
	uint8_t reg;
}irq_cfg_param_t;

// All times in ms.
const uint32_t mpl3115a2_bus_timeout_ms = 100;		// one transfer, to issue or to land
const uint32_t mpl3115a2_startup_ms = 100;			// in mpl3115a2_init_state before the first access
const uint32_t mpl3115a2_reset_poll_ms = 10;		// after reset, and between RST polls
const uint32_t mpl3115a2_phase_timeout_ms = 1000;	// start up, or one whole measurement
const uint32_t mpl3115a2_period_ms = 5000;			// between measurements
const uint32_t mpl3115a2_conversion_poll_ms = 100;	// before each look at the status register
const uint32_t mpl3115a2_error_backoff_ms = 100;	// in mpl3115a2_error_state before starting again

// The bus handle and device address are no longer here. They belong
// to the transport and go to its constructor.
typedef struct mpl3115a2_param_t{
	mpl3115a2_mode_t mode;
	uint8_t oversample_ratio;	// one of MPL3115A2_CTRL_REG1_OSx
}mpl3115a2_param_t;

template <typename TTransport>
class mpl3115a2 : public SensorStateMachine<TTransport, mpl3115a2_state_t> {
	static_assert(std::is_base_of<ISensorTransport, TTransport>::value,
			"mpl3115a2<TTransport>: TTransport must derive from ISensorTransport");
	typedef SensorStateMachine<TTransport, mpl3115a2_state_t> base;
protected:
	// Names from a template base class have to be brought in before
	// they can be used unqualified.
	using base::enter;
	using base::fail;
	using base::elapsed;
	using base::sleepRemaining;
	using base::issued;
	using base::landed;
	using base::finished;
	using base::errorCleared;
	using base::sleep;
	using base::_state;
	using base::last_update;
public:
	template <typename... TArgs>
	explicit mpl3115a2(const mpl3115a2_param_t &param, TArgs&&... transportArgs);
	virtual ~mpl3115a2() {}

	void main(uint32_t nowMs);
	bool newData();
	float getTemp();		// degrees C. NAN until the first reading
	float getPressure();	// hPa, in barometer mode. NAN in altimeter mode
	float getAltitude();	// m, in altimeter mode. NAN in barometer mode

	// Sets the interrupt enables (ctrl_reg4) and which pin each goes
	// to (ctrl_reg5). Written to the chip during start up, or before
	// the next measurement if the chip is already running.
	void SetIRQ(irq_en_param_t irq_en, irq_cfg_param_t irq_cfg);

private:

	mpl3115a2_param_t _param;
	uint32_t phase_start;	// when start up, or the current measurement, began
	bool _new_data,_EnableIRQ,_irq_changed;

	float _temperature;
	float _pressure;
	float _altitude;

	uint8_t _who_am_i;

	// The transport fills this in after main() has returned, so it
	// has to be a member and not a local.
	uint8_t dataReg[5];

	typedef union{
		struct{
			uint8_t SBYB : 1;
			uint8_t OST : 1;
			uint8_t RST : 1;
			uint8_t OS : 3;
			uint8_t : 1;
			uint8_t ALT : 1;
		}bit;
		uint8_t reg;
	}ctrl_reg1_t;
	ctrl_reg1_t ctrl_reg1;

	// ctrl_reg4 - interrupt enable register
	irq_en_param_t ctrl_reg4;

	// ctrl_reg5 - interrupt configuration register
	irq_cfg_param_t ctrl_reg5;

	typedef union{
			struct{
				uint8_t :1;
				uint8_t TDR :1;
				uint8_t PDR :1;
				uint8_t PTDR : 1;
				uint8_t : 1;
				uint8_t TOW : 1;
				uint8_t POW : 1;
				uint8_t PTOW : 1;
			}bit;
			uint8_t reg;
	}dr_status_reg_t;
	dr_status_reg_t dr_status;

	typedef union{
				struct{
					uint8_t TDEFE : 1;
					uint8_t PDEFE : 1;
					uint8_t DREM : 1;
					uint8_t : 5;
				}bit;
				uint8_t reg;
	}pt_data_cfg_reg_t;
	pt_data_cfg_reg_t pt_data_cfg;

	bool phaseExpired(uint32_t nowMs) const;
	void _getConversion();

};

/*
 * Templates have to be visible wherever they are used, so the code
 * that was in mpl3115a2.cpp lives below.
 */

template <typename TTransport>
template <typename... TArgs>
mpl3115a2<TTransport>::mpl3115a2(const mpl3115a2_param_t &param, TArgs&&... transportArgs)
	: base(mpl3115a2_init_state, mpl3115a2_error_state, mpl3115a2_bus_timeout_ms, std::forward<TArgs>(transportArgs)...) {
	_param=param;
	_new_data=false;
	_temperature=NAN;
	_pressure=NAN;
	_altitude=NAN;
	_EnableIRQ=false;
	_irq_changed=false;
	_who_am_i=0;
	phase_start=0;
	ctrl_reg1.reg=0;
	ctrl_reg4.reg=0;
	ctrl_reg5.reg=0;
	dr_status.reg=0;
	pt_data_cfg.reg=0;
}

template <typename TTransport>
void mpl3115a2<TTransport>::main(uint32_t nowMs){
	switch(_state){
	case mpl3115a2_init_state:
		if(elapsed(nowMs,mpl3115a2_startup_ms)){
			if(this->checkDevice()){
				enter(mpl3115a2_whoami_state,nowMs);
			} else {
				fail(nowMs);
			}
		} else {
			sleepRemaining(nowMs,mpl3115a2_startup_ms);
		}
		break;
	case mpl3115a2_whoami_state:
		issued(this->readRegs(mpl3115a2_who_am_i_addr,&_who_am_i,1),mpl3115a2_wait_whoami_state,nowMs);
		break;
	case mpl3115a2_wait_whoami_state:
		if(landed(nowMs)){
			if(_who_am_i==mpl3115a2_who_am_i){
				enter(mpl3115a2_reset_state,nowMs);
			} else {
				fail(nowMs);
			}
		}
		break;
	case mpl3115a2_reset_state:
		ctrl_reg1.reg=0;
		ctrl_reg1.bit.RST=1;
		issued(this->writeReg(mpl3115a2_ctrl_reg1_addr,ctrl_reg1.reg),mpl3115a2_wait_reset_state,nowMs);
		break;
	case mpl3115a2_wait_reset_state:
		// The chip can reset before it acknowledges this write, so an
		// error here is not a fault. Whether the reset happened is
		// settled by the RST polls that follow.
		if(finished(nowMs)){
			phase_start=nowMs;
			enter(mpl3115a2_reset_settle_state,nowMs);
		}
		break;
	case mpl3115a2_reset_settle_state:
		if(!elapsed(nowMs,mpl3115a2_reset_poll_ms)){
			sleepRemaining(nowMs,mpl3115a2_reset_poll_ms);
		} else if(phaseExpired(nowMs)){
			fail(nowMs); // RST never cleared
		} else {
			enter(mpl3115a2_read_reset_state,nowMs);
		}
		break;
	case mpl3115a2_read_reset_state:
		issued(this->readRegs(mpl3115a2_ctrl_reg1_addr,&ctrl_reg1.reg,1),mpl3115a2_wait_read_reset_state,nowMs);
		break;
	case mpl3115a2_wait_read_reset_state:
		// The chip does not answer while it is resetting, so a failed
		// read means the same as RST still being set: ask again.
		if(finished(nowMs)){
			if(this->lastOpFailed()||ctrl_reg1.bit.RST==1){
				enter(mpl3115a2_reset_settle_state,nowMs);
			} else {
				enter(mpl3115a2_initializing_state,nowMs);
			}
		}
		break;
	case mpl3115a2_initializing_state:
		ctrl_reg1.reg=0;
		ctrl_reg1.bit.ALT=_param.mode;
		ctrl_reg1.bit.OS=_param.oversample_ratio;
		issued(this->writeReg(mpl3115a2_ctrl_reg1_addr,ctrl_reg1.reg),mpl3115a2_wait_ctrl_reg1_state,nowMs);
		break;
	case mpl3115a2_wait_ctrl_reg1_state:
		if(landed(nowMs)){
			enter(mpl3115a2_pt_data_cfg_state,nowMs);
		}
		break;
	case mpl3115a2_pt_data_cfg_state:
		pt_data_cfg.reg=0;
		pt_data_cfg.bit.DREM=1;
		pt_data_cfg.bit.PDEFE=1;
		pt_data_cfg.bit.TDEFE=1;
		issued(this->writeReg(mpl3115a2_pt_data_cfg_addr,pt_data_cfg.reg),mpl3115a2_wait_pt_data_cfg_state,nowMs);
		break;
	case mpl3115a2_wait_pt_data_cfg_state:
		if(landed(nowMs)){
			if(_EnableIRQ){
				enter(mpl3115a2_ctrl_reg5_state,nowMs);
			} else {
				enter(mpl3115a2_done_state,nowMs);
			}
		}
		break;
	case mpl3115a2_ctrl_reg5_state:
		// Routing first, so an interrupt is never enabled on the wrong pin.
		_irq_changed=false;
		issued(this->writeReg(mpl3115a2_ctrl_reg5_addr,ctrl_reg5.reg),mpl3115a2_wait_ctrl_reg5_state,nowMs);
		break;
	case mpl3115a2_wait_ctrl_reg5_state:
		if(landed(nowMs)){
			enter(mpl3115a2_ctrl_reg4_state,nowMs);
		}
		break;
	case mpl3115a2_ctrl_reg4_state:
		issued(this->writeReg(mpl3115a2_ctrl_reg4_addr,ctrl_reg4.reg),mpl3115a2_wait_ctrl_reg4_state,nowMs);
		break;
	case mpl3115a2_wait_ctrl_reg4_state:
		if(landed(nowMs)){
			enter(mpl3115a2_done_state,nowMs);
		}
		break;
	case mpl3115a2_done_state:
		if(_irq_changed){ // SetIRQ() was called while running. The wait starts again afterwards
			enter(mpl3115a2_ctrl_reg5_state,nowMs);
		} else if(elapsed(nowMs,mpl3115a2_period_ms)){
			phase_start=nowMs;
			enter(mpl3115a2_initiate_one_shot_measurement,nowMs);
		} else {
			sleepRemaining(nowMs,mpl3115a2_period_ms);
		}
		break;
	case mpl3115a2_initiate_one_shot_measurement:
		issued(this->readRegs(mpl3115a2_ctrl_reg1_addr,&ctrl_reg1.reg,1),mpl3115a2_wait_ost_state,nowMs);
		break;
	case mpl3115a2_wait_ost_state:
		if(landed(nowMs)){
			if(ctrl_reg1.bit.OST==1){ // a measurement is already running
				enter(mpl3115a2_wait_one_shot_measurement,nowMs);
			} else {
				enter(mpl3115a2_write_ost_state,nowMs);
			}
		}
		break;
	case mpl3115a2_write_ost_state:
		ctrl_reg1.bit.OST=1;
		issued(this->writeReg(mpl3115a2_ctrl_reg1_addr,ctrl_reg1.reg),mpl3115a2_wait_write_ost_state,nowMs);
		break;
	case mpl3115a2_wait_write_ost_state:
		if(landed(nowMs)){
			enter(mpl3115a2_wait_one_shot_measurement,nowMs);
		}
		break;
	case mpl3115a2_wait_one_shot_measurement:
		if(!elapsed(nowMs,mpl3115a2_conversion_poll_ms)){
			sleepRemaining(nowMs,mpl3115a2_conversion_poll_ms);
		} else if(phaseExpired(nowMs)){
			fail(nowMs); // the conversion never finished
		} else {
			enter(mpl3115a2_status_state,nowMs);
		}
		break;
	case mpl3115a2_status_state:
		issued(this->readRegs(mpl3115a2_status_addr,&dr_status.reg,1),mpl3115a2_wait_status_state,nowMs);
		break;
	case mpl3115a2_wait_status_state:
		if(landed(nowMs)){
			if(dr_status.bit.PTDR!=0){
				enter(mpl3115a2_conversion_state,nowMs);
			} else {
				enter(mpl3115a2_wait_one_shot_measurement,nowMs);
			}
		}
		break;
	case mpl3115a2_conversion_state:
		issued(this->readRegs(mpl3115a2_out_p_msb_addr,dataReg,5),mpl3115a2_wait_conversion_state,nowMs);
		break;
	case mpl3115a2_wait_conversion_state:
		if(landed(nowMs)){
			_getConversion();
			_new_data=true;
			enter(mpl3115a2_done_state,nowMs);
		}
		break;
	case mpl3115a2_error_state:
		if(errorCleared(nowMs,mpl3115a2_error_backoff_ms)){
			enter(mpl3115a2_init_state,nowMs);
		}
		break;
	default:
		enter(mpl3115a2_init_state,nowMs);
		break;
	}
}

template <typename TTransport>
bool mpl3115a2<TTransport>::phaseExpired(uint32_t nowMs) const{
	return (nowMs-phase_start)>mpl3115a2_phase_timeout_ms;
}

// The same three bytes hold pressure in barometer mode and altitude
// in altimeter mode, so only one of the two is ever valid.
template <typename TTransport>
void mpl3115a2<TTransport>::_getConversion(){
	int16_t t;

	if(_param.mode==mpl3115a2_altimeter_mode){
		// signed, metres, in 1/65536ths once left justified to 32 bits
		int32_t alt;
		alt = (int32_t)(uint32_t(dataReg[0]) << 24 | uint32_t(dataReg[1]) << 16 |
			  uint32_t(dataReg[2]) << 8);
		_altitude=float(alt) / 65536.0f;
		_pressure=NAN;
	} else {
		uint32_t pressure;
		pressure=0;
		pressure|= (uint32_t(dataReg[0])<<16);
		pressure|= (uint32_t(dataReg[1])<<8);
		pressure|= (uint32_t(dataReg[2]));
		_pressure=(float(pressure) / 6400.0f);
		_altitude=NAN;
	}

	t = (int16_t)((uint16_t(dataReg[3]) << 8) | (uint16_t(dataReg[4])));
	_temperature=(float(t)/256.0f);
}

template <typename TTransport>
bool mpl3115a2<TTransport>::newData(){
	return _new_data;
}

template <typename TTransport>
float mpl3115a2<TTransport>::getTemp(){
	_new_data=false;
	return _temperature;
}

template <typename TTransport>
float mpl3115a2<TTransport>::getPressure(){
	_new_data=false;
	return _pressure;
}

template <typename TTransport>
float mpl3115a2<TTransport>::getAltitude(){
	_new_data=false;
	return _altitude;
}

template <typename TTransport>
void mpl3115a2<TTransport>::SetIRQ(irq_en_param_t irq_en, irq_cfg_param_t irq_cfg){
	ctrl_reg4.reg=irq_en.reg;
	ctrl_reg5.reg=irq_cfg.reg;
	_EnableIRQ=true;
	_irq_changed=true;
}

} /* namespace mpl3115a2 */

#endif /* MPL3115A2_H_ */
