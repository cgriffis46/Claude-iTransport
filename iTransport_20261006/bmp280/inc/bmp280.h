/*
 * bmp280.h
 *
 *  Created on: Aug 9, 2025
 *      Author: coryg
 *
 *  BMP280 pressure / temperature sensor, non-blocking state machine.
 *
 *  The bus is chosen by the template argument. bmp280<TTransport>
 *  inherits from TTransport, which must be an ISensorTransport
 *  (itransport/inc/ISensorTransport.h):
 *
 *      bmp280<Stm32HalI2CTransport>  sensor(param, &hi2c1, bmp280_i2c_addr_2, i2c1Mutex);
 *      bmp280<Stm32HalSPITransport>  sensor(param, &hspi1, GPIOB, GPIO_PIN_6);
 *
 *  Everything after param goes to the transport's own constructor.
 *  This file includes no HAL and no RTOS header, so it builds for any
 *  target the transport builds for.
 *
 *  Call main(now) every pass of the loop. It never waits on the bus:
 *  each register access is one state that issues the transfer and one
 *  state that waits for it to land. When a state has nothing to do it
 *  calls sleep(), an empty stub here. Override sleep() for an OS (see
 *  xbmp280.h) to give up the CPU for that long.
 *
 *  The state bookkeeping, sleep() and the issue/wait helpers are
 *  inherited from SensorStateMachine (isensor/inc), shared with the
 *  other drivers.
 */

#ifndef BMP280_H_
#define BMP280_H_

#include <stdint.h>
#include <math.h>
#include <type_traits>
#include <utility>
#include "ISensorTransport.h"
#include "SensorStateMachine.h"

namespace BMP280 {

#define BMP280_ID 0x58

typedef enum bmp280_i2c_addr_t{
	bmp280_i2c_addr_1 = 0x76,
	bmp280_i2c_addr_2 = 0x77,
}bmp280_i2c_addr_t;

// defines for register addresses

typedef enum bmp280_reg_addr_t{
	bmp280_reg_id_addr = 0xD0,
	bmp280_reg_reset_addr = 0xE0,
	bmp280_reg_status_addr = 0xF3,
	bmp280_reg_ctrl_meas_addr = 0xF4,
	bmp280_reg_config_addr = 0xF5,
	bmp280_reg_press_msb_addr = 0xF7,
	bmp280_reg_press_lsb_addr = 0xF8,
	bmp280_reg_press_xlsb_addr = 0xF9,
	bmp280_reg_temp_msb_addr = 0xFA,
	bmp280_reg_temp_lsb_addr = 0xFB,
	bmp280_reg_temp_xlsb_addr = 0xFC,
	bmp280_reg_cal_addr = 0x88
}bmp280_reg_addr_t;

typedef uint8_t bmp280_reg8_t;
typedef int32_t BMP280_S32_t;
typedef uint32_t BMP280_U32_t;
const bmp280_reg8_t bmp280_reset_value = 0xB6;

// pressure oversampling register settings
typedef enum bmp280_osrs_p_t{
	bmp280_osrs_p_skip = 0b000,
	bmp280_osrs_p_1x = 0b001,
	bmp280_osrs_p_2x = 0b010,
	bmp280_osrs_p_4x = 0b011,
	bmp280_osrs_p_8x = 0b100,
	bmp280_osrs_p_16x = 0b101
}bmp280_osrs_p_t;

// temperature oversampling register settings
typedef enum bmp280_osrs_t_t {
	bmp280_osrs_t_skip = 0b000,
	bmp280_osrs_t_1x = 0b001,
	bmp280_osrs_t_2x = 0b010,
	bmp280_osrs_t_4x = 0b011,
	bmp280_osrs_t_8x = 0b100,
	bmp280_osrs_t_16x = 0b101
}bmp280_osrs_t_t;

typedef enum bmp280_mode_t{
	bmp280_sleep_mode = 0b00,
	bmp280_forced_mode = 0b10,
	bmp280_normal_mode = 0b11
}bmp280_mode_t;

typedef enum bmp280_t_sb{
	bmp280_sb_05 = 0b000,
	bmp280_sb_62 = 0b001,
	bmp280_sb_125 = 0b010,
	bmp280_sb_250 = 0b011,
	bmp280_sb_500 = 0b100,
	bmp280_sb_1000 = 0b101,
	bmp280_sb_2000 = 0b110,
	bmp280_sb_4000 = 0b111
}bmp280_t_sb;

// IIR filter coefficient
typedef enum bmp280_filter_t{
	bmp280_filter_off = 0b000,
	bmp280_filter_x2 = 0b001,
	bmp280_filter_x4 = 0b010,
	bmp280_filter_x8 = 0b011,
	bmp280_filter_x16 = 0b100
}bmp280_filter_t;

typedef enum bmp280_spi_en{
	bmp280_spi_en_off = 0b0,
	bmp280_spi_en_on = 0b1
}bmp280_spi_en;

// Every bus access is two states: one that issues the transfer and a
// wait_ state that it lands in. The transport never blocks, so the
// state machine has to remember which half it is in.
typedef enum bmp280_state_t{
	bmp280_init,					// check a device answers on the bus
	bmp280_reset,					// issue the soft reset
	bmp280_wait_reset,
	bmp280_reset_settle,			// pause after reset and between im_update polls
	bmp280_read_im_update,			// issue the status read
	bmp280_wait_im_update,			// im_update clears once the trim values are copied
	bmp280_read_id,
	bmp280_wait_id,
	bmp280_read_cal,
	bmp280_wait_cal,
	bmp280_write_config,
	bmp280_wait_config,
	bmp280_write_ctrl_meas,
	bmp280_wait_ctrl_meas,
	bmp280_sleeping,				// between measurements
	bmp280_start_measurement,		// issue the status read: is the chip idle
	bmp280_wait_idle,
	bmp280_force_measurement,		// issue the forced mode write
	bmp280_wait_force,
	bmp280_measuring,				// conversion time
	bmp280_read_measuring,			// issue the status read: has it finished
	bmp280_wait_measuring,
	bmp280_read_data,
	bmp280_wait_data,
	bmp280_convert_measurement,
	bmp280_error					// pause, then start again from bmp280_init
}bmp280_state_t;

// All times in ms.
const uint32_t bmp280_bus_timeout_ms = 100;		// one transfer, to issue or to land
const uint32_t bmp280_im_update_poll_ms = 10;	// after reset, and between im_update polls
const uint32_t bmp280_phase_timeout_ms = 1000;	// start up, or one whole measurement
const uint32_t bmp280_conversion_ms = 200;		// before first checking the measuring bit
const uint32_t bmp280_period_ms = 1000;			// between measurements
const uint32_t bmp280_error_backoff_ms = 100;	// in bmp280_error before starting again

// The bus handle and device address are no longer here. They belong
// to the transport and go to its constructor.
typedef struct bmp280_param_t{
	bmp280_osrs_p_t _osrs_p_t;
	bmp280_osrs_t_t _osrs_t_t;
	bmp280_mode_t _mode_t;
	bmp280_t_sb _t_sb;
	bmp280_filter_t _filter_t;
}bmp280_param_t;

template <typename TTransport>
class bmp280 : public SensorStateMachine<TTransport, bmp280_state_t> {
	static_assert(std::is_base_of<ISensorTransport, TTransport>::value,
			"bmp280<TTransport>: TTransport must derive from ISensorTransport");
	typedef SensorStateMachine<TTransport, bmp280_state_t> base;
protected:
	// Names from a template base class have to be brought in before
	// they can be used unqualified.
	using base::enter;
	using base::fail;
	using base::elapsed;
	using base::sleepRemaining;
	using base::issued;
	using base::landed;
	using base::errorCleared;
	using base::sleep;
	using base::_state;
	using base::last_update;
public:
	template <typename... TArgs>
	explicit bmp280(const bmp280_param_t &param, TArgs&&... transportArgs);
	virtual ~bmp280() {}

	void main(uint32_t nowMs);

	bool newData();
	bool GetTemperature(double *t);	// degrees C. false until the first reading
	bool GetPressure(double *p);	// Pa. false until the first reading

protected:

	// register definitions

	typedef union{
		struct{
			uint8_t spi3w_en : 1;
			uint8_t : 1;
			uint8_t filter : 3;
			uint8_t t_sb : 3;
		}bit;
		uint8_t reg;
	}bmp280_config_reg_t;
	bmp280_config_reg_t config;

	typedef union{
		struct{
			uint8_t mode : 2;
			uint8_t osrs_p : 3;
			uint8_t osrs_t : 3;
		}bit;
		uint8_t reg;
	}bmp280_ctrl_meas_reg_t;
	bmp280_ctrl_meas_reg_t ctrl_meas;

	typedef union{
		struct{
			uint8_t im_update : 1;
			uint8_t : 2;
			uint8_t measuring : 1;
			uint8_t : 4;
		}bit;
		uint8_t reg;
	}bmp280_status_reg_t;
	bmp280_status_reg_t status;

	bmp280_reg8_t _id; // whoami

	BMP280_S32_t t_fine;
	unsigned short dig_T1;
	signed short dig_T2, dig_T3;
	unsigned short dig_P1;
	signed short dig_P2, dig_P3, dig_P4, dig_P5, dig_P6, dig_P7, dig_P8, dig_P9;

	uint32_t phase_start;	// when start up, or the current measurement, began

	bool _newData;

	double _temp,_pressure;
	BMP280_S32_t adc_t, adc_p;
	bmp280_param_t _param;

private:

	// The transport fills these in after main() has returned, so they
	// have to be members and not locals.
	uint8_t calReg[24];
	uint8_t dataReg[6];

	bool phaseExpired(uint32_t nowMs) const;

	void parseCal();
	double bmp280_compensate_T_double(BMP280_S32_t adc_T);
	double bmp280_compensate_P_double(BMP280_S32_t adc_P);

};

/*
 * Templates have to be visible wherever they are used, so the code
 * that was in bmp280.cpp lives below.
 */

template <typename TTransport>
template <typename... TArgs>
bmp280<TTransport>::bmp280(const bmp280_param_t &param, TArgs&&... transportArgs)
	: base(bmp280_init, bmp280_error, bmp280_bus_timeout_ms, std::forward<TArgs>(transportArgs)...) {
	_param=param;
	config.reg=0;
	ctrl_meas.reg=0;
	status.reg=0;
	_id=0;
	t_fine=0;
	adc_t=0;
	adc_p=0;
	_newData=false;
	_temp=NAN;
	_pressure=NAN;
	phase_start=0;
}

template <typename TTransport>
void bmp280<TTransport>::main(uint32_t nowMs){
	switch (_state){
		case bmp280_init:
			_newData=false;
			if(this->checkDevice()){
				enter(bmp280_reset,nowMs);
			} else {
				fail(nowMs);
			}
			break;
		case bmp280_reset:
			issued(this->writeReg(bmp280_reg_reset_addr,bmp280_reset_value),bmp280_wait_reset,nowMs);
			break;
		case bmp280_wait_reset:
			if(landed(nowMs)){
				phase_start=nowMs;
				enter(bmp280_reset_settle,nowMs);
			}
			break;
		case bmp280_reset_settle:
			if(!elapsed(nowMs,bmp280_im_update_poll_ms)){
				sleepRemaining(nowMs,bmp280_im_update_poll_ms);
			} else if(phaseExpired(nowMs)){
				fail(nowMs); // im_update never cleared
			} else {
				enter(bmp280_read_im_update,nowMs);
			}
			break;
		case bmp280_read_im_update:
			issued(this->readRegs(bmp280_reg_status_addr,&status.reg,1),bmp280_wait_im_update,nowMs);
			break;
		case bmp280_wait_im_update:
			// im_update clears when BMP280 completes copying calibration trim values from eeprom into the registers.
			if(landed(nowMs)){
				if(status.bit.im_update!=1){
					enter(bmp280_read_id,nowMs);
				} else {
					enter(bmp280_reset_settle,nowMs);
				}
			}
			break;
		case bmp280_read_id:
			issued(this->readRegs(bmp280_reg_id_addr,&_id,1),bmp280_wait_id,nowMs);
			break;
		case bmp280_wait_id:
			if(landed(nowMs)){
				if(_id==BMP280_ID){
					enter(bmp280_read_cal,nowMs);
				} else {
					fail(nowMs);
				}
			}
			break;
		case bmp280_read_cal:
			issued(this->readRegs(bmp280_reg_cal_addr,calReg,24),bmp280_wait_cal,nowMs);
			break;
		case bmp280_wait_cal:
			if(landed(nowMs)){
				parseCal();
				enter(bmp280_write_config,nowMs);
			}
			break;
		case bmp280_write_config:
			config.bit.t_sb=_param._t_sb;
			config.bit.spi3w_en=bmp280_spi_en_off; // itransport's SPI is 4 wire
			config.bit.filter=_param._filter_t;
			issued(this->writeReg(bmp280_reg_config_addr,config.reg),bmp280_wait_config,nowMs);
			break;
		case bmp280_wait_config:
			if(landed(nowMs)){
				enter(bmp280_write_ctrl_meas,nowMs);
			}
			break;
		case bmp280_write_ctrl_meas:
			ctrl_meas.bit.mode=_param._mode_t;
			ctrl_meas.bit.osrs_p=_param._osrs_p_t;
			ctrl_meas.bit.osrs_t=_param._osrs_t_t;
			issued(this->writeReg(bmp280_reg_ctrl_meas_addr,ctrl_meas.reg),bmp280_wait_ctrl_meas,nowMs);
			break;
		case bmp280_wait_ctrl_meas:
			if(landed(nowMs)){
				enter(bmp280_sleeping,nowMs);
			}
			break;
		case bmp280_sleeping:
			if(elapsed(nowMs,bmp280_period_ms)){
				phase_start=nowMs;
				enter(bmp280_start_measurement,nowMs);
			} else {
				sleepRemaining(nowMs,bmp280_period_ms);
			}
			break;
		case bmp280_start_measurement:
			issued(this->readRegs(bmp280_reg_status_addr,&status.reg,1),bmp280_wait_idle,nowMs);
			break;
		case bmp280_wait_idle:
			if(landed(nowMs)){
				if(status.bit.measuring!=1){
					enter(bmp280_force_measurement,nowMs);
				} else if(phaseExpired(nowMs)){
					fail(nowMs);
				} else { // bmp280 is already measuring. ask again
					sleep(1);
					enter(bmp280_start_measurement,nowMs);
				}
			}
			break;
		case bmp280_force_measurement:
			_param._mode_t=bmp280_forced_mode;
			ctrl_meas.bit.mode=_param._mode_t;
			ctrl_meas.bit.osrs_p=_param._osrs_p_t;
			ctrl_meas.bit.osrs_t=_param._osrs_t_t;
			issued(this->writeReg(bmp280_reg_ctrl_meas_addr,ctrl_meas.reg),bmp280_wait_force,nowMs);
			break;
		case bmp280_wait_force:
			if(landed(nowMs)){
				enter(bmp280_measuring,nowMs);
			}
			break;
		case bmp280_measuring:
			if(elapsed(nowMs,bmp280_conversion_ms)){
				enter(bmp280_read_measuring,nowMs);
			} else {
				sleepRemaining(nowMs,bmp280_conversion_ms);
			}
			break;
		case bmp280_read_measuring:
			issued(this->readRegs(bmp280_reg_status_addr,&status.reg,1),bmp280_wait_measuring,nowMs);
			break;
		case bmp280_wait_measuring:
			if(landed(nowMs)){
				if(status.bit.measuring!=1){
					enter(bmp280_read_data,nowMs);
				} else if(phaseExpired(nowMs)){
					fail(nowMs);
				} else { // still converting. ask again
					sleep(1);
					enter(bmp280_read_measuring,nowMs);
				}
			}
			break;
		case bmp280_read_data:
			issued(this->readRegs(bmp280_reg_press_msb_addr,dataReg,6),bmp280_wait_data,nowMs); // burst read temp/pressure data
			break;
		case bmp280_wait_data:
			if(landed(nowMs)){
				enter(bmp280_convert_measurement,nowMs);
			}
			break;
		case bmp280_convert_measurement:
			adc_p = dataReg[0];
			adc_p = adc_p<<8;
			adc_p |= dataReg[1];
			adc_p = adc_p<<8;
			adc_p |= dataReg[2];
			adc_p = adc_p>>4;

			adc_t = dataReg[3];
			adc_t = adc_t<<8;
			adc_t |= dataReg[4];
			adc_t = adc_t<<8;
			adc_t |= dataReg[5];
			adc_t = adc_t>>4;

			_temp = bmp280_compensate_T_double(adc_t); // must run first. sets t_fine
			_pressure = bmp280_compensate_P_double(adc_p);
			_newData = true;
			enter(bmp280_sleeping,nowMs);
			break;
		case bmp280_error:
			if(errorCleared(nowMs,bmp280_error_backoff_ms)){
				enter(bmp280_init,nowMs);
			}
			break;
		default: // if we don't know what state we're in, re-init
			enter(bmp280_init,nowMs);
			break;
	}
}

template <typename TTransport>
bool bmp280<TTransport>::phaseExpired(uint32_t nowMs) const{
	return (nowMs-phase_start)>bmp280_phase_timeout_ms;
}

template <typename TTransport>
void bmp280<TTransport>::parseCal(){
	dig_T1=calReg[1]<<8;
	dig_T1|=calReg[0];
	dig_T2=calReg[3]<<8;
	dig_T2|=calReg[2];
	dig_T3=calReg[5]<<8;
	dig_T3|=calReg[4];

	dig_P1=calReg[7]<<8;
	dig_P1|=calReg[6];
	dig_P2=calReg[9]<<8;
	dig_P2|=calReg[8];
	dig_P3=calReg[11]<<8;
	dig_P3|=calReg[10];
	dig_P4=calReg[13]<<8;
	dig_P4|=calReg[12];
	dig_P5=calReg[15]<<8;
	dig_P5|=calReg[14];
	dig_P6=calReg[17]<<8;
	dig_P6|=calReg[16];
	dig_P7=calReg[19]<<8;
	dig_P7|=calReg[18];
	dig_P8=calReg[21]<<8;
	dig_P8|=calReg[20];
	dig_P9=calReg[23]<<8;
	dig_P9|=calReg[22];
}

/* Taken from BMP280 Datasheet */
template <typename TTransport>
double bmp280<TTransport>::bmp280_compensate_T_double(BMP280_S32_t adc_T){
	double var1, var2, T;
	var1 = (((double)adc_T)/16384.0 - ((double)dig_T1)/1024.0) *((double)dig_T2);
	var2 = ((((double)adc_T)/131072.0 - ((double)dig_T1)/8192.0)*(((double)adc_T)/131072.0 - ((double)dig_T1)/8192.0))*((double)dig_T3);
	t_fine = (BMP280_S32_t)(var1+var2);
	T=(var1+var2)/5120.0;
	return T;
}

/* Taken from BMP280 Datasheet */
template <typename TTransport>
double bmp280<TTransport>::bmp280_compensate_P_double(BMP280_S32_t adc_P){
	double var1, var2, P;
	var1 = ((double)t_fine/2.0)-64000.0;
	var2 = var1*var1*((double)dig_P6)/32768.0;
	var2 = var2+var1 * ((double)dig_P5)*2.0;
	var2 = (var2/4.0)+(((double)dig_P4)*65536.0);
	var1 = (((double)dig_P3)*var1*var1/524288.0+((double)dig_P2)*var1)/524288.0;
	var1 = (1.0+var1/32768.0)*((double)dig_P1);
	if(var1 == 0.0){
		return 0;
	}
	P=1048576.0 - (double)adc_P;
	P=(P-(var2/4096.0))*6250.0/var1;
	var1 = ((double)dig_P9)*P*P/2147483648.0;
	var2 = P*((double)dig_P8)/32768.0;
	P = P+(var1+var2+((double)dig_P7))/16.0;
	return P;
}

template <typename TTransport>
bool bmp280<TTransport>::newData(){
	return _newData;
}

template <typename TTransport>
bool bmp280<TTransport>::GetTemperature(double *t){
	*t = _temp;
	_newData = false;
	return !isnan(_temp);
}

template <typename TTransport>
bool bmp280<TTransport>::GetPressure(double *p){
	*p = _pressure;
	_newData = false;
	return !isnan(_pressure);
}

} /* namespace BMP280 */

#endif /* BMP280_H_ */
