/*
 * AHT20.h
 *
 *  Created on: May 31, 2025
 *      Author: coryg
 *
 *  AHT20 temperature / humidity sensor, non-blocking state machine.
 *
 *  The bus is chosen by the template argument. AHT20<TTransport>
 *  inherits from TTransport, which must be an ISensorTransport
 *  (itransport/inc/ISensorTransport.h):
 *
 *      AHT20<Stm32HalI2CTransport>  sensor(&hi2c1, AHT20_I2C_Addr, i2c1Mutex);
 *
 *  The constructor arguments all go to the transport's own
 *  constructor. This file includes no HAL and no RTOS header.
 *
 *  The AHT20 has no registers. It takes commands and is read back as
 *  a plain run of bytes, so this driver uses the transport's
 *  writeBytes() and readBytes().
 *
 *  Call main(now) every pass of the loop. It never waits on the bus:
 *  each command and each read is one state that issues it and one
 *  state that waits for it to land. When a state has nothing to do it
 *  calls sleep(), an empty stub here. Override sleep() for an OS (see
 *  xAHT20.h) to give up the CPU for that long.
 *
 *  The state bookkeeping, sleep() and the issue/wait helpers are
 *  inherited from SensorStateMachine (isensor/inc), shared with the
 *  other drivers.
 *
 *  Measures once every aht20_period_ms, and at once when
 *  StartMeasurement() asks.
 */

#ifndef AHT20_H_
#define AHT20_H_

#include <stdint.h>
#include <math.h>
#include <type_traits>
#include <utility>
#include "ISensorTransport.h"
#include "SensorStateMachine.h"

const uint8_t AHT20_I2C_Addr = 0x38;
const uint8_t AHT20_MeasurementCmd = 0xAC;
const uint8_t AHT20_Status_Reg = 0x71;
const uint8_t AHT20_Init_Cmd = 0xBE;
const uint8_t AHT20_Init_High = 0x08;
const uint8_t AHT20_Init_Low = 0x00;
const uint8_t AHT20_MeasurementCmdHigh = 0x33;
const uint8_t AHT20_MeasurementCmdLow = 0x00;
const uint8_t AHT20_SoftResetCmd = 0xBA;

const uint8_t AHT20_Status_Busy = 0b10000000;		// a measurement is in progress
const uint8_t AHT20_Status_Calibrated = 0x18;		// both bits set once the chip is ready for use

// Every bus access is two states: one that issues it and a wait
// state that it lands in.
typedef enum AHT20_state_t{
	aht20_init,						// check a device answers
	aht_soft_reset,					// issue the soft reset
	aht_wait_soft_reset,
	aht_reset,						// pause while the reset, or the re-init, takes effect
	aht_status_cmd,					// issue the status command
	aht_wait_status_cmd,
	aht_read_status,				// issue the 1 byte read
	aht_wait_status,
	aht_reinit,						// issue the initialisation command
	aht_wait_reinit,
	aht_sleep,						// between measurements
	aht_start_measurement,			// issue the measurement command
	aht_wait_start_measurement,
	aht_measuring,					// conversion time
	aht_read_measurement,			// issue the 7 byte read
	aht_wait_measurement,
	aht_busy_pause,					// the chip was still busy. pause, then read again
	aht_error						// pause, then start again from aht20_init
}AHT20_state_t;

// All times in ms.
const uint32_t aht20_bus_timeout_ms = 100;		// one bus operation, to issue or to land
const uint32_t aht20_reset_ms = 100;			// after a soft reset or a re-init
const uint32_t aht20_phase_timeout_ms = 1000;	// start up, or one whole measurement
const uint32_t aht20_period_ms = 1000;			// between measurements
const uint32_t aht20_request_poll_ms = 100;		// longest sleep while a StartMeasurement() could arrive
const uint32_t aht20_conversion_ms = 100;		// before the first read
const uint32_t aht20_busy_poll_ms = 10;			// between reads while the chip is still busy
const uint32_t aht20_error_backoff_ms = 100;	// in aht_error before starting again

template <typename TTransport>
class AHT20 : public SensorStateMachine<TTransport, AHT20_state_t> {
	static_assert(std::is_base_of<ISensorTransport, TTransport>::value,
			"AHT20<TTransport>: TTransport must derive from ISensorTransport");
	typedef SensorStateMachine<TTransport, AHT20_state_t> base;
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
	explicit AHT20(TArgs&&... transportArgs);
	virtual ~AHT20() {}

	void StartMeasurement(); // ask for a measurement now. only taken up between measurements
	void main(uint32_t nowMs);
	bool newData();	// true once per measurement
	void getTempHumidity(float *temp, float *humidity); // returns the values already read from the sensor. NAN until the first reading, and after a failure
private:
	uint8_t status_word;
	uint32_t phase_start;	// when start up, or the current measurement, began
	bool _start_measure;
	bool _newData;
	float _temp,_humidity;

	// The transport fills this in after main() has returned, so it
	// has to be a member and not a local.
	uint8_t dataReg[7];

	// A failure leaves no valid reading, as it did when a failed read
	// came back as NAN.
	void onFail() override { _temp=NAN; _humidity=NAN; }

	bool phaseExpired(uint32_t nowMs) const;
	void convert();
	static uint8_t crc8(const uint8_t *data, int len);
};

/*
 * Templates have to be visible wherever they are used, so the code
 * that was in AHT20.cpp lives below.
 */

template <typename TTransport>
template <typename... TArgs>
AHT20<TTransport>::AHT20(TArgs&&... transportArgs)
	: base(aht20_init, aht_error, aht20_bus_timeout_ms, std::forward<TArgs>(transportArgs)...) {
	status_word=0;
	phase_start=0;
	_start_measure = false;
	_newData=false;
	_temp=NAN;
	_humidity=NAN;
}

template <typename TTransport>
void AHT20<TTransport>::StartMeasurement(){
	if(aht_sleep==_state){
		_start_measure=true;
	}
}

template <typename TTransport>
void AHT20<TTransport>::main(uint32_t nowMs){
	switch(_state){
	case aht20_init:
		_start_measure=false;
		if(this->checkDevice()){ // check if device exists
			enter(aht_soft_reset,nowMs);
		}
		else { // device doesn't exist. go to error
			fail(nowMs);
		}
		break;
	case aht_soft_reset:
		issued(this->writeBytes(&AHT20_SoftResetCmd,1),aht_wait_soft_reset,nowMs);
		break;
	case aht_wait_soft_reset:
		if(landed(nowMs)){
			phase_start=nowMs;
			enter(aht_reset,nowMs);
		}
		break;
	case aht_reset:
		// wait for AHT20 to complete reset.
		if(!elapsed(nowMs,aht20_reset_ms)){
			sleepRemaining(nowMs,aht20_reset_ms);
		} else if(phaseExpired(nowMs)){
			fail(nowMs); // re-initialised over and over and still not calibrated
		} else {
			enter(aht_status_cmd,nowMs);
		}
		break;
	case aht_status_cmd:
		issued(this->writeBytes(&AHT20_Status_Reg,1),aht_wait_status_cmd,nowMs);
		break;
	case aht_wait_status_cmd:
		if(landed(nowMs)){
			enter(aht_read_status,nowMs);
		}
		break;
	case aht_read_status:
		issued(this->readBytes(&status_word,1),aht_wait_status,nowMs);
		break;
	case aht_wait_status:
		if(landed(nowMs)){
			// if the calibration bits are not both set, reinitialize the AHT20 sensor
			if((status_word&AHT20_Status_Calibrated)!=AHT20_Status_Calibrated){
				enter(aht_reinit,nowMs);
			}
			else {
				enter(aht_sleep,nowMs);
			}
		}
		break;
	case aht_reinit:
		{
			// The transport takes its own copy, so a local is fine here.
			const uint8_t buf[3]={AHT20_Init_Cmd,AHT20_Init_High,AHT20_Init_Low};
			issued(this->writeBytes(buf,3),aht_wait_reinit,nowMs);
		}
		break;
	case aht_wait_reinit:
		if(landed(nowMs)){
			enter(aht_reset,nowMs);
		}
		break;
	case aht_sleep:
		if((_start_measure==true)||elapsed(nowMs,aht20_period_ms)){
			_start_measure=false;
			phase_start=nowMs;
			enter(aht_start_measurement,nowMs);
		} else {
			// No longer than aht20_request_poll_ms at a time, so a
			// StartMeasurement() request is seen without waiting out
			// the whole period.
			uint32_t left=aht20_period_ms-(nowMs-last_update);
			sleep(left<aht20_request_poll_ms?left:aht20_request_poll_ms);
		}
		break;
	case aht_start_measurement:
		{
			const uint8_t buf[3]={AHT20_MeasurementCmd,AHT20_MeasurementCmdHigh,AHT20_MeasurementCmdLow};
			issued(this->writeBytes(buf,3),aht_wait_start_measurement,nowMs);
		}
		break;
	case aht_wait_start_measurement:
		if(landed(nowMs)){
			enter(aht_measuring,nowMs);
		}
		break;
	case aht_measuring:
		// wait for conversion to complete
		if(elapsed(nowMs,aht20_conversion_ms)){
			enter(aht_read_measurement,nowMs);
		} else {
			sleepRemaining(nowMs,aht20_conversion_ms);
		}
		break;
	case aht_read_measurement:
		issued(this->readBytes(dataReg,7),aht_wait_measurement,nowMs);
		break;
	case aht_wait_measurement:
		if(landed(nowMs)){
			if((dataReg[0]&AHT20_Status_Busy)>0){ // sensor is busy
				if(phaseExpired(nowMs)){
					fail(nowMs);
				} else {
					enter(aht_busy_pause,nowMs);
				}
			} else if(crc8(dataReg,6)!=dataReg[6]){ // check crc
				fail(nowMs);
			} else {
				convert();
				_newData=true;
				enter(aht_sleep,nowMs);
			}
		}
		break;
	case aht_busy_pause:
		if(elapsed(nowMs,aht20_busy_poll_ms)){
			enter(aht_read_measurement,nowMs);
		} else {
			sleepRemaining(nowMs,aht20_busy_poll_ms);
		}
		break;
	case aht_error:
		if(errorCleared(nowMs,aht20_error_backoff_ms)){
			enter(aht20_init,nowMs);
		}
		break;
	default:
		enter(aht20_init,nowMs);
		break;
	}
}

template <typename TTransport>
bool AHT20<TTransport>::phaseExpired(uint32_t nowMs) const{
	return (nowMs-phase_start)>aht20_phase_timeout_ms;
}

template <typename TTransport>
void AHT20<TTransport>::convert(){
	uint32_t t,h;
	uint8_t scratch;

	// convert humidity value
	h = dataReg[1];
	h = h<<8;
	scratch = dataReg[2];
	h|=scratch;
	h = h<<4;
	scratch = dataReg[3]&0b11110000;
	scratch=scratch>>4;
	h|=scratch;

	_humidity = (float(h)/float(0x100000))*100;

	// convert temp value
	t=0;
	scratch = dataReg[3]&0b00001111;
	t|=scratch;
	t<<=8;
	scratch = dataReg[4];
	t|=scratch;
	t<<=8;
	scratch=dataReg[5];
	t|=scratch;

	_temp = (float(t)/float(0x100000))*200 - 50;
}

/**
 * Performs a CRC8 calculation on the supplied values.
 *
 * @param data  Pointer to the data to use when calculating the CRC8.
 * @param len   The number of bytes in 'data'.
 *
 * @return The computed CRC8 value.
 */
template <typename TTransport>
uint8_t AHT20<TTransport>::crc8(const uint8_t *data, int len) {
  /*
   *
   * CRC-8 formula from page 14 of SHT spec pdf
   *
   * Test data 0xBE, 0xEF should yield 0x92
   *
   * Initialization data 0xFF
   * Polynomial 0x31 (x8 + x5 +x4 +1)
   * Final XOR 0x00
   */

  const uint8_t POLYNOMIAL(0x31);
  uint8_t crc(0xFF);

  for (int j = len; j; --j) {
    crc ^= *data++;

    for (int i = 8; i; --i) {
      crc = (crc & 0x80) ? (crc << 1) ^ POLYNOMIAL : (crc << 1);
    }
  }
  return crc;
}

template <typename TTransport>
void AHT20<TTransport>::getTempHumidity(float *temp, float *humidity){
	*temp = _temp;
	*humidity = _humidity;
}

template <typename TTransport>
bool AHT20<TTransport>::newData(){
	bool old = _newData;
	_newData=false;
	return old;
}

#endif /* AHT20_H_ */
