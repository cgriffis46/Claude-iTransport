/*
 * BME280.h
 *
 *  Created on: Nov 16, 2025
 *      Author: coryg
 *
 *  BME280 pressure / temperature / humidity sensor, non-blocking
 *  state machine.
 *
 *  The bus is chosen by the template argument. bme280<TTransport>
 *  inherits from TTransport, which must be an ISensorTransport
 *  (itransport/inc/ISensorTransport.h):
 *
 *      bme280<Stm32HalI2CTransport>  sensor(param, &hi2c1, bme280_i2c_addr_2, i2c1Mutex);
 *      bme280<Stm32HalSPITransport>  sensor(param, &hspi1, GPIOB, GPIO_PIN_6, spi1Mutex);
 *
 *  Everything after param goes to the transport's own constructor.
 *  This file includes no HAL and no RTOS header, so it builds for any
 *  target the transport builds for.
 *
 *  Call main(now) every pass of the loop. It never waits on the bus:
 *  each register access is one state that issues the transfer and one
 *  state that waits for it to land. When a state has nothing to do it
 *  calls sleep(), an empty stub here. Override sleep() for an OS (see
 *  xBME280.h) to give up the CPU for that long.
 *
 *  The state bookkeeping, sleep() and the issue/wait helpers are
 *  inherited from SensorStateMachine (isensor/inc), shared with the
 *  other drivers.
 */

#ifndef BME280_H_
#define BME280_H_

#include <stdint.h>
#include <math.h>
#include <type_traits>
#include <utility>
#include "ISensorTransport.h"
#include "SensorStateMachine.h"

namespace BME280 {

#define bme280_id 0x60

typedef enum bme280_i2c_addr_t{
	bme280_i2c_addr_1 = 0x76,
	bme280_i2c_addr_2 = 0x77,
}bme280_i2c_addr_t;

// defines for register addresses

typedef enum bme280_reg_addr_t{
	bme280_reg_id_addr = 0xD0,
	bme280_reg_reset_addr = 0xE0,
	bme280_reg_ctrl_hum_addr = 0xF2,
	bme280_reg_status_addr = 0xF3,
	bme280_reg_ctrl_meas_addr = 0xF4,
	bme280_reg_config_addr = 0xF5,
	bme280_reg_press_msb_addr = 0xF7,
	bme280_reg_press_lsb_addr = 0xF8,
	bme280_reg_press_xlsb_addr = 0xF9,
	bme280_reg_temp_msb_addr = 0xFA,
	bme280_reg_temp_lsb_addr = 0xFB,
	bme280_reg_temp_xlsb_addr = 0xFC,
	bme280_reg_hum_msb_addr = 0xFD,
	bme280_reg_hum_lsb_addr = 0xFE,
	bme280_reg_cal_addr = 0x88,
	bme280_reg_dig_H1_addr = 0xA1,
	bme280_reg_dig_H2_addr = 0xE1
}bme280_reg_addr_t;

typedef uint8_t bme280_reg8_val_t;
typedef int32_t bme280_S32_t;
typedef uint32_t bme280_U32_t;
const bme280_reg8_val_t bme280_reset_value = 0xB6;

// pressure oversampling register settings
typedef enum bme280_osrs_p_t{
 bme280_osrs_p_skip = 0b000,
 bme280_osrs_p_1x = 0b001,
 bme280_osrs_p_2x = 0b010,
 bme280_osrs_p_4x = 0b011,
 bme280_osrs_p_8x = 0b100,
 bme280_osrs_p_16x = 0b101
}bme280_osrs_p_t;

// temperature oversampling register settings
typedef enum bme280_osrs_t_t{
 bme280_osrs_t_skip = 0b000,
 bme280_osrs_t_1x = 0b001,
 bme280_osrs_t_2x = 0b010,
 bme280_osrs_t_4x = 0b011,
 bme280_osrs_t_8x = 0b100,
 bme280_osrs_t_16x = 0b101
}bme280_osrs_t_t;

typedef enum bme280_osrs_h_t{
	 bme280_osrs_h_skip = 0b000,
	 bme280_osrs_h_1x = 0b001,
	 bme280_osrs_h_2x = 0b010,
	 bme280_osrs_h_4x = 0b011,
	 bme280_osrs_h_8x = 0b100,
	 bme280_osrs_h_16x = 0b101
}bme280_osrs_h_t;

typedef enum bme280_mode_t{
	bme280_sleep_mode = 0b00,
	bme280_forced_mode = 0b10,
	bme280_normal_mode = 0b11
}bme280_mode_t;

// standby time in normal mode. The last two differ from the BMP280,
// where the same settings mean 2000 ms and 4000 ms.
typedef enum bme280_sb_t{
	bme280_sb_05 = 0b000,
	bme280_sb_62 = 0b001,
	bme280_sb_125 = 0b010,
	bme280_sb_250 = 0b011,
	bme280_sb_500 = 0b100,
	bme280_sb_1000 = 0b101,
	bme280_sb_10 = 0b110,
	bme280_sb_20 = 0b111
}bme280_sb_t;

// IIR filter coefficient
typedef enum bme280_filter_t{
	bme280_filter_off = 0b000,
	bme280_filter_x2 = 0b001,
	bme280_filter_x4 = 0b010,
	bme280_filter_x8 = 0b011,
	bme280_filter_x16 = 0b100
}bme280_filter_t;

typedef enum bme280_spi_en{
	bme280_spi_en_off = 0b0,
	bme280_spi_en_on = 0b1
}bme280_spi_en;

// Every bus access is two states: one that issues the transfer and a
// wait_ state that it lands in. The transport never blocks, so the
// state machine has to remember which half it is in.
typedef enum bme280_state_t{
	bme280_init,
	bme280_reset,					// issue the soft reset
	bme280_wait_reset,
	bme280_reset_settle,			// pause after reset
	bme280_read_id,
	bme280_wait_id,
	bme280_im_update_pause,			// pause between im_update polls
	bme280_read_im_update,			// issue the status read
	bme280_wait_im_update,			// im_update clears once the trim values are copied
	bme280_read_cal,				// temperature and pressure trim, 0x88
	bme280_wait_cal,
	bme280_read_cal_h1,				// humidity trim, first byte, 0xA1
	bme280_wait_cal_h1,
	bme280_read_cal_h2,				// humidity trim, the rest, 0xE1
	bme280_wait_cal_h2,
	bme280_write_ctrl_hum,
	bme280_wait_ctrl_hum,
	bme280_write_config,
	bme280_wait_config,
	bme280_write_ctrl_meas,
	bme280_wait_ctrl_meas,
	bme280_sleeping,				// between measurements
	bme280_start_measurement,		// issue the status read: is the chip idle
	bme280_wait_idle,
	bme280_force_measurement,		// issue the forced mode write
	bme280_wait_force,
	bme280_measuring,				// conversion time
	bme280_read_measuring,			// issue the status read: has it finished
	bme280_wait_measuring,
	bme280_read_data,
	bme280_wait_data,
	bme280_convert_measurement,
	bme280_error					// pause, then start again from bme280_init
}bme280_state_t;

// All times in ms.
const uint32_t bme280_bus_timeout_ms = 100;		// one transfer, to issue or to land
const uint32_t bme280_reset_settle_ms = 100;	// after the soft reset, before asking for the ID
const uint32_t bme280_im_update_poll_ms = 10;	// between im_update polls
const uint32_t bme280_phase_timeout_ms = 1000;	// start up, or one whole measurement
const uint32_t bme280_sleep_ms = 100;			// between measurements
const uint32_t bme280_conversion_ms = 100;		// before first checking the measuring bit
const uint32_t bme280_error_backoff_ms = 100;	// in bme280_error before starting again

// The bus handle and device address are no longer here. They belong
// to the transport and go to its constructor.
typedef struct bme280_param_t{
	bme280_osrs_p_t _osrs_p_t;
	bme280_osrs_t_t _osrs_t_t;
	bme280_osrs_h_t _osrs_h_t;
	bme280_mode_t _mode_t;
	bme280_sb_t _sb_t;
	bme280_filter_t _filter_t;
}bme280_param_t;

template <typename TTransport>
class bme280 : public SensorStateMachine<TTransport, bme280_state_t> {
	static_assert(std::is_base_of<ISensorTransport, TTransport>::value,
			"bme280<TTransport>: TTransport must derive from ISensorTransport");
	typedef SensorStateMachine<TTransport, bme280_state_t> base;
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
	explicit bme280(const bme280_param_t &param, TArgs&&... transportArgs);
	virtual ~bme280() {}

	void main(uint32_t nowMs);

	bool newData();	// true once per measurement

	bool GetTemperature(double *t);	// degrees C. false until the first reading
	bool GetPressure(double *p);	// Pa. false until the first reading
	bool GetHumidity(double *h);	// %RH. false until the first reading

protected:

	uint8_t _id;
	bme280_S32_t t_fine;

	unsigned char dig_H1;
	signed short dig_H2;
	unsigned char dig_H3;
	signed short dig_H4;
	signed short dig_H5;
	signed char dig_H6;

	unsigned short dig_T1;
	signed short dig_T2, dig_T3;
	unsigned short dig_P1;
	signed short dig_P2, dig_P3, dig_P4, dig_P5, dig_P6, dig_P7, dig_P8, dig_P9;

	uint32_t phase_start;	// when start up, or the current measurement, began

	bool _newData;
	bme280_param_t _param;

	typedef union{
		struct{
			uint8_t spi3w_en : 1;
			uint8_t : 1;
			uint8_t filter : 3;
			uint8_t t_sb : 3;
		}bit;
		uint8_t reg;
	}config_reg_t;
	config_reg_t _config;

	typedef union{
		struct{
			uint8_t mode : 2;   // data aquisition mode
			uint8_t osrs_p : 3; // pressure oversampling rate
			uint8_t osrs_t : 3; // temperature oversamping rate
		}bit;
		uint8_t reg;
	}ctrl_meas_reg_t;
	ctrl_meas_reg_t _ctrl_meas;

	typedef union{
		struct{
			uint8_t im_update : 1; // true when the sensor is loading calibration values.
			uint8_t : 2;
			uint8_t measuring : 1; // true when the sensor is measuring
			uint8_t : 4;
		}bit;
		uint8_t reg;
	}status_reg_t;
	status_reg_t _status;

	typedef union{
		struct{
			uint8_t osrs_h : 3; // humidity oversampling rate
			uint8_t : 5;
		}bit;
		uint8_t reg;
	}ctrl_hum_reg_t;
	ctrl_hum_reg_t _ctrl_hum;

	double _temp,_pressure,_humidity;
	bme280_S32_t adc_t, adc_p, adc_h;

private:

	// The transport fills these in after main() has returned, so they
	// have to be members and not locals.
	uint8_t calReg[24];
	uint8_t calH1Reg;
	uint8_t calHReg[7];
	uint8_t dataReg[8];

	bool phaseExpired(uint32_t nowMs) const;

	void parseCal();
	void parseHumCal();
	double bme280_compensate_T_double(bme280_S32_t adc_T);
	double bme280_compensate_P_double(bme280_S32_t adc_P);
	double bme280_compensate_H_double(bme280_S32_t adc_H);

};

/*
 * Templates have to be visible wherever they are used, so the code
 * that was in BME280.cpp lives below.
 */

template <typename TTransport>
template <typename... TArgs>
bme280<TTransport>::bme280(const bme280_param_t &param, TArgs&&... transportArgs)
	: base(bme280_init, bme280_error, bme280_bus_timeout_ms, std::forward<TArgs>(transportArgs)...) {
	_param=param;
	_config.reg=0;
	_ctrl_meas.reg=0;
	_status.reg=0;
	_ctrl_hum.reg=0;
	_id=0;
	t_fine=0;
	adc_t=0;
	adc_p=0;
	adc_h=0;
	_newData=false;
	_temp=NAN;
	_pressure=NAN;
	_humidity=NAN;
	phase_start=0;
}

template <typename TTransport>
void bme280<TTransport>::main(uint32_t nowMs){
	switch (_state){
		case bme280_init:
			_newData=false;
			enter(bme280_reset,nowMs);
			break;
		case bme280_reset:
			issued(this->writeReg(bme280_reg_reset_addr,bme280_reset_value),bme280_wait_reset,nowMs);
			break;
		case bme280_wait_reset:
			if(landed(nowMs)){
				phase_start=nowMs;
				enter(bme280_reset_settle,nowMs);
			}
			break;
		case bme280_reset_settle:
			// wait 100ms after issuing Reset
			if(elapsed(nowMs,bme280_reset_settle_ms)){
				enter(bme280_read_id,nowMs);
			} else {
				sleepRemaining(nowMs,bme280_reset_settle_ms);
			}
			break;
		case bme280_read_id:
			issued(this->readRegs(bme280_reg_id_addr,&_id,1),bme280_wait_id,nowMs);
			break;
		case bme280_wait_id:
			if(landed(nowMs)){
				if(_id==bme280_id){
					enter(bme280_im_update_pause,nowMs);
				} else {
					fail(nowMs);
				}
			}
			break;
		case bme280_im_update_pause:
			if(!elapsed(nowMs,bme280_im_update_poll_ms)){
				sleepRemaining(nowMs,bme280_im_update_poll_ms);
			} else if(phaseExpired(nowMs)){
				fail(nowMs); // im_update never cleared
			} else {
				enter(bme280_read_im_update,nowMs);
			}
			break;
		case bme280_read_im_update:
			issued(this->readRegs(bme280_reg_status_addr,&_status.reg,1),bme280_wait_im_update,nowMs);
			break;
		case bme280_wait_im_update:
			// im_update clears when BME280 completes copying calibration trim values from eeprom into the registers.
			if(landed(nowMs)){
				if(_status.bit.im_update!=1){
					enter(bme280_read_cal,nowMs);
				} else {
					enter(bme280_im_update_pause,nowMs);
				}
			}
			break;
		case bme280_read_cal:
			issued(this->readRegs(bme280_reg_cal_addr,calReg,24),bme280_wait_cal,nowMs);
			break;
		case bme280_wait_cal:
			if(landed(nowMs)){
				parseCal();
				enter(bme280_read_cal_h1,nowMs);
			}
			break;
		case bme280_read_cal_h1:
			issued(this->readRegs(bme280_reg_dig_H1_addr,&calH1Reg,1),bme280_wait_cal_h1,nowMs);
			break;
		case bme280_wait_cal_h1:
			if(landed(nowMs)){
				enter(bme280_read_cal_h2,nowMs);
			}
			break;
		case bme280_read_cal_h2:
			issued(this->readRegs(bme280_reg_dig_H2_addr,calHReg,7),bme280_wait_cal_h2,nowMs);
			break;
		case bme280_wait_cal_h2:
			if(landed(nowMs)){
				parseHumCal();
				enter(bme280_write_ctrl_hum,nowMs);
			}
			break;
		case bme280_write_ctrl_hum:
			// Must come before ctrl_meas: the chip only takes up a new
			// ctrl_hum when ctrl_meas is next written.
			_ctrl_hum.bit.osrs_h=_param._osrs_h_t;
			issued(this->writeReg(bme280_reg_ctrl_hum_addr,_ctrl_hum.reg),bme280_wait_ctrl_hum,nowMs);
			break;
		case bme280_wait_ctrl_hum:
			if(landed(nowMs)){
				enter(bme280_write_config,nowMs);
			}
			break;
		case bme280_write_config:
			_config.bit.t_sb=_param._sb_t;
			_config.bit.spi3w_en=bme280_spi_en_off; // itransport's SPI is 4 wire
			_config.bit.filter=_param._filter_t;
			issued(this->writeReg(bme280_reg_config_addr,_config.reg),bme280_wait_config,nowMs);
			break;
		case bme280_wait_config:
			if(landed(nowMs)){
				enter(bme280_write_ctrl_meas,nowMs);
			}
			break;
		case bme280_write_ctrl_meas:
			_ctrl_meas.bit.mode=_param._mode_t;
			_ctrl_meas.bit.osrs_p=_param._osrs_p_t;
			_ctrl_meas.bit.osrs_t=_param._osrs_t_t;
			issued(this->writeReg(bme280_reg_ctrl_meas_addr,_ctrl_meas.reg),bme280_wait_ctrl_meas,nowMs);
			break;
		case bme280_wait_ctrl_meas:
			if(landed(nowMs)){
				enter(bme280_sleeping,nowMs);
			}
			break;
		case bme280_sleeping:
			if(elapsed(nowMs,bme280_sleep_ms)){
				phase_start=nowMs;
				enter(bme280_start_measurement,nowMs);
			} else {
				sleepRemaining(nowMs,bme280_sleep_ms);
			}
			break;
		case bme280_start_measurement:
			issued(this->readRegs(bme280_reg_status_addr,&_status.reg,1),bme280_wait_idle,nowMs);
			break;
		case bme280_wait_idle:
			if(landed(nowMs)){
				if(_status.bit.measuring!=1){ // bme280 is not already measuring
					enter(bme280_force_measurement,nowMs);
				} else if(phaseExpired(nowMs)){
					fail(nowMs);
				} else { // ask again
					sleep(1);
					enter(bme280_start_measurement,nowMs);
				}
			}
			break;
		case bme280_force_measurement:
			// start forced measurement
			_param._mode_t=bme280_forced_mode;
			_ctrl_meas.bit.mode=_param._mode_t;
			_ctrl_meas.bit.osrs_p=_param._osrs_p_t;
			_ctrl_meas.bit.osrs_t=_param._osrs_t_t;
			issued(this->writeReg(bme280_reg_ctrl_meas_addr,_ctrl_meas.reg),bme280_wait_force,nowMs);
			break;
		case bme280_wait_force:
			if(landed(nowMs)){
				enter(bme280_measuring,nowMs);
			}
			break;
		case bme280_measuring:
			if(elapsed(nowMs,bme280_conversion_ms)){
				enter(bme280_read_measuring,nowMs);
			} else {
				sleepRemaining(nowMs,bme280_conversion_ms);
			}
			break;
		case bme280_read_measuring:
			issued(this->readRegs(bme280_reg_status_addr,&_status.reg,1),bme280_wait_measuring,nowMs);
			break;
		case bme280_wait_measuring:
			if(landed(nowMs)){
				if(_status.bit.measuring!=1){
					enter(bme280_read_data,nowMs);
				} else if(phaseExpired(nowMs)){
					fail(nowMs);
				} else { // still converting. ask again
					sleep(1);
					enter(bme280_read_measuring,nowMs);
				}
			}
			break;
		case bme280_read_data:
			issued(this->readRegs(bme280_reg_press_msb_addr,dataReg,8),bme280_wait_data,nowMs); // burst read temp/pressure/humidity data
			break;
		case bme280_wait_data:
			if(landed(nowMs)){
				enter(bme280_convert_measurement,nowMs);
			}
			break;
		case bme280_convert_measurement:
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

			adc_h = dataReg[6];
			adc_h = adc_h<<8;
			adc_h |= dataReg[7];

			_temp = bme280_compensate_T_double(adc_t); // must run first. sets t_fine
			_pressure = bme280_compensate_P_double(adc_p);
			_humidity = bme280_compensate_H_double(adc_h);
			_newData = true;
			enter(bme280_sleeping,nowMs);
			break;
		case bme280_error:
			if(errorCleared(nowMs,bme280_error_backoff_ms)){
				enter(bme280_init,nowMs);
			}
			break;
		default: // if we don't know what state we're in, re-init
			enter(bme280_init,nowMs);
			break;
	}
}

template <typename TTransport>
bool bme280<TTransport>::phaseExpired(uint32_t nowMs) const{
	return (nowMs-phase_start)>bme280_phase_timeout_ms;
}

template <typename TTransport>
void bme280<TTransport>::parseCal(){
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

template <typename TTransport>
void bme280<TTransport>::parseHumCal(){
	uint8_t temp;

	dig_H1 = calH1Reg;

	dig_H2 = calHReg[1];
	dig_H2 = dig_H2<<8;
	dig_H2 |= calHReg[0];

	dig_H3 = calHReg[2];

	// dig_H4 and dig_H5 are signed 12 bit values. The byte holding
	// the top 8 bits carries the sign, so it is read as signed.
	dig_H4 = (signed char)calHReg[3];
	dig_H4 = dig_H4*16;
	dig_H4 |= (calHReg[4]&0b00001111);

	temp = calHReg[4]&0b11110000;
	temp = temp>>4;
	dig_H5 = (signed char)calHReg[5];//0xE6
	dig_H5 = dig_H5*16;
	dig_H5 |= temp;

	dig_H6 = (signed char)calHReg[6];
}

/* Taken from bme280 Datasheet */
template <typename TTransport>
double bme280<TTransport>::bme280_compensate_T_double(bme280_S32_t adc_T){
	double var1, var2, T;
	var1 = (((double)adc_T)/16384.0 - ((double)dig_T1)/1024.0) *((double)dig_T2);
	var2 = ((((double)adc_T)/131072.0 - ((double)dig_T1)/8192.0)*(((double)adc_T)/131072.0 - ((double)dig_T1)/8192.0))*((double)dig_T3);
	t_fine = (bme280_S32_t)(var1+var2);
	T=(var1+var2)/5120.0;
	return T;
}

/* Taken from bme280 Datasheet */
template <typename TTransport>
double bme280<TTransport>::bme280_compensate_P_double(bme280_S32_t adc_P){
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

/* Taken from bme280 Datasheet */
template <typename TTransport>
double bme280<TTransport>::bme280_compensate_H_double(bme280_S32_t adc_H){
	double var_H;
	var_H = (((double)t_fine) - 76800.0);
	var_H = (adc_H-(((double)dig_H4)*64.0+((double)dig_H5)/16384.0*var_H))*(((double)dig_H2)/65536.0*(1.0+((double)dig_H6)/67108864.0*var_H*(1.0+((double)dig_H3)/67108864.0*var_H)));
	var_H = var_H*(1.0-((double)dig_H1)*var_H/524288.0);
	if(var_H>100.0) {
		var_H = 100.0;
	} else if (var_H<0.0){
		var_H = 0.0;
	}
	return var_H;
}

template <typename TTransport>
bool bme280<TTransport>::newData(){
	bool _new = _newData;
	_newData = false;
	return _new;
}

template <typename TTransport>
bool bme280<TTransport>::GetTemperature(double *t){
	*t = _temp;
	_newData = false;
	return !isnan(_temp);
}

template <typename TTransport>
bool bme280<TTransport>::GetPressure(double *p){
	*p = _pressure;
	_newData=false;
	return !isnan(_pressure);
}

template <typename TTransport>
bool bme280<TTransport>::GetHumidity(double *h){
	*h = _humidity;
	_newData=false;
	return !isnan(_humidity);
}

} /* namespace BME280 */

#endif /* BME280_H_ */
