/*
 * DS18B20.h
 *
 *  Created on: Feb 1, 2026
 *      Author: coryg
 *
 *  DS18B20 1-Wire temperature sensor, non-blocking state machine.
 *  Replaces DS18B20.cpp and DS18B20_NB.h/.cpp.
 *
 *  The bus is chosen by the template argument. ds18b20<TTransport>
 *  inherits from TTransport, which must be an iTransportOneWire
 *  (itransport/inc/iTransportOneWire.h):
 *
 *      ds18b20<Stm32HalOneWireTransport>  sensor(&huart1, uart1_mutexHandle);
 *
 *  The constructor arguments all go to the transport's own
 *  constructor. This file includes no HAL and no RTOS header.
 *
 *  Call main(now) every pass of the loop. It never waits on the bus:
 *  each reset, write and read is one state that issues it and one
 *  state that waits for it to land. When a state has nothing to do it
 *  calls sleep(), an empty stub here. Override sleep() for an OS (see
 *  xDS18B20.h) to give up the CPU for that long.
 *
 *  The state bookkeeping, sleep() and the issue/wait helpers are
 *  inherited from SensorStateMachine (isensor/inc), shared with the
 *  other drivers.
 *
 *  One device on the wire, addressed with Skip ROM, powered from VDD
 *  (not parasite power).
 */

#ifndef DS18B20_H_
#define DS18B20_H_

#include <stdint.h>
#include <math.h>
#include <type_traits>
#include <utility>
#include "iTransportOneWire.h"
#include "SensorStateMachine.h"

namespace DS18B20 {

typedef enum DS18B20_cmd_t{
	DS18B20_cmd_skip_rom = 0xCC,			// ROM-CMD
	DS18B20_cmd_convert_t = 0x44,			// F-CMD
	DS18B20_cmd_read_scratchpad = 0xBE		// F-CMD
}DS18B20_cmd_t;

// scratchpad layout
typedef enum DS18B20_scratchpad_t{
	DS18B20_scratchpad_temp_lsb = 0,
	DS18B20_scratchpad_temp_msb = 1,
	DS18B20_scratchpad_config = 4,			// resolution, bits 6:5
	DS18B20_scratchpad_crc = 8,
	DS18B20_scratchpad_len = 9
}DS18B20_scratchpad_t;

// Every bus operation is two states: one that issues it and one that
// it lands in. A measurement is two 1-Wire transactions, each begun
// by a reset: start the conversion, then, once it has had time to
// finish, read the result back.
enum DS18B20_state_t{
	DS18B20_init_state,
	DS18B20_done_state,					// between measurements
	DS18B20_reset_state,				// issue the reset that begins Convert T
	DS18B20_presence_state,				// did a device answer
	DS18B20_convert_state,				// issue Skip ROM, Convert T
	DS18B20_wait_convert_state,
	DS18B20_converting_state,			// conversion time
	DS18B20_read_reset_state,			// issue the reset that begins the read
	DS18B20_read_presence_state,
	DS18B20_read_cmd_state,				// issue Skip ROM, Read Scratchpad
	DS18B20_wait_read_cmd_state,
	DS18B20_read_temp_state,			// issue the 9 byte read
	DS18B20_wait_read_temp_state,		// check it and work out the temperature
	DS18B20_err_state					// pause, then start again from DS18B20_init_state
};

// All times in ms.
const uint32_t DS18B20_bus_timeout_ms = 100;	// one bus operation, to issue or to land
const uint32_t DS18B20_conversion_ms = 750;		// 12 bit conversion, the longest there is
const uint32_t DS18B20_period_ms = 1000;		// from one measurement starting to the next
const uint32_t DS18B20_error_backoff_ms = 100;	// in DS18B20_err_state before starting again

template <typename TTransport>
class ds18b20 : public SensorStateMachine<TTransport, DS18B20_state_t> {
	static_assert(std::is_base_of<iTransportOneWire, TTransport>::value,
			"ds18b20<TTransport>: TTransport must derive from iTransportOneWire");
	typedef SensorStateMachine<TTransport, DS18B20_state_t> base;
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
	explicit ds18b20(TArgs&&... transportArgs);
	virtual ~ds18b20() {}

	void main(uint32_t nowMs);
	bool newData();
	void getTemp(float *temp);	// degrees C. NAN until the first reading

protected:

	bool _newData;
	uint32_t cycle_start;	// when the current measurement began
	float _temp;

private:

	// The transport fills this in after main() has returned, so it
	// has to be a member and not a local.
	uint8_t scratchpad[DS18B20_scratchpad_len];

	bool scratchpadValid() const;
	float scratchpadTemp() const;
	static uint8_t crc8(const uint8_t *data, uint8_t len);
};

/*
 * Templates have to be visible wherever they are used, so the code
 * that was in DS18B20.cpp lives below.
 */

template <typename TTransport>
template <typename... TArgs>
ds18b20<TTransport>::ds18b20(TArgs&&... transportArgs)
	: base(DS18B20_init_state, DS18B20_err_state, DS18B20_bus_timeout_ms, std::forward<TArgs>(transportArgs)...) {
	_newData=false;
	_temp=NAN;
	cycle_start=0;
	for(uint8_t i=0;i<DS18B20_scratchpad_len;i++){
		scratchpad[i]=0;
	}
}

template <typename TTransport>
void ds18b20<TTransport>::main(uint32_t nowMs){
	switch(_state){
	case DS18B20_init_state:
		cycle_start=nowMs;
		enter(DS18B20_done_state,nowMs);
		break;
	case DS18B20_done_state:
		if((nowMs-cycle_start)>=DS18B20_period_ms){
			cycle_start=nowMs;
			enter(DS18B20_reset_state,nowMs);
		} else {
			sleepRemaining(cycle_start,nowMs,DS18B20_period_ms);
		}
		break;
	case DS18B20_reset_state:
		issued(this->reset(),DS18B20_presence_state,nowMs);
		break;
	case DS18B20_presence_state:
		if(landed(nowMs)){
			if(this->presence()){
				enter(DS18B20_convert_state,nowMs);
			} else {
				fail(nowMs); // No presence pulse detected
			}
		}
		break;
	case DS18B20_convert_state:
		{
			// The transport takes its own copy, so a local is fine here.
			const uint8_t cmd[2]={DS18B20_cmd_skip_rom,DS18B20_cmd_convert_t};
			issued(this->writeBytes(cmd,2),DS18B20_wait_convert_state,nowMs);
		}
		break;
	case DS18B20_wait_convert_state:
		if(landed(nowMs)){
			enter(DS18B20_converting_state,nowMs);
		}
		break;
	case DS18B20_converting_state:
		// Read any sooner and the scratchpad still holds the previous
		// conversion, or 85 C straight after power up.
		if(elapsed(nowMs,DS18B20_conversion_ms)){
			enter(DS18B20_read_reset_state,nowMs);
		} else {
			sleepRemaining(nowMs,DS18B20_conversion_ms);
		}
		break;
	case DS18B20_read_reset_state:
		issued(this->reset(),DS18B20_read_presence_state,nowMs);
		break;
	case DS18B20_read_presence_state:
		if(landed(nowMs)){
			if(this->presence()){
				enter(DS18B20_read_cmd_state,nowMs);
			} else {
				fail(nowMs);
			}
		}
		break;
	case DS18B20_read_cmd_state:
		{
			const uint8_t cmd[2]={DS18B20_cmd_skip_rom,DS18B20_cmd_read_scratchpad};
			issued(this->writeBytes(cmd,2),DS18B20_wait_read_cmd_state,nowMs);
		}
		break;
	case DS18B20_wait_read_cmd_state:
		if(landed(nowMs)){
			enter(DS18B20_read_temp_state,nowMs);
		}
		break;
	case DS18B20_read_temp_state:
		issued(this->readBytes(scratchpad,DS18B20_scratchpad_len),DS18B20_wait_read_temp_state,nowMs);
		break;
	case DS18B20_wait_read_temp_state:
		if(landed(nowMs)){
			if(scratchpadValid()){
				_temp=scratchpadTemp();
				_newData=true;
				enter(DS18B20_done_state,nowMs);
			} else {
				fail(nowMs);
			}
		}
		break;
	case DS18B20_err_state:
		if(errorCleared(nowMs,DS18B20_error_backoff_ms)){
			enter(DS18B20_init_state,nowMs);
		}
		break;
	default: // if we don't know what state we're in, re-init
		enter(DS18B20_init_state,nowMs);
		break;
	}
}

// The CRC catches a corrupted read and a device that has gone away
// (the line then reads all 1s). All 0s has to be caught separately:
// the CRC of nine zero bytes is itself zero.
template <typename TTransport>
bool ds18b20<TTransport>::scratchpadValid() const{
	bool allZero=true;
	for(uint8_t i=0;i<DS18B20_scratchpad_len;i++){
		if(scratchpad[i]!=0){
			allZero=false;
		}
	}
	if(allZero){
		return false;
	}
	return crc8(scratchpad,DS18B20_scratchpad_crc)==scratchpad[DS18B20_scratchpad_crc];
}

template <typename TTransport>
float ds18b20<TTransport>::scratchpadTemp() const{
	// Signed: temperatures below zero are two's complement.
	int16_t Temp = (int16_t)(((uint16_t)scratchpad[DS18B20_scratchpad_temp_msb]<<8)|scratchpad[DS18B20_scratchpad_temp_lsb]);

	// Below 12 bit resolution the lowest bits are undefined.
	switch((scratchpad[DS18B20_scratchpad_config]>>5)&0x03){
	case 0: // 9 bit
		Temp &= ~0x07;
		break;
	case 1: // 10 bit
		Temp &= ~0x03;
		break;
	case 2: // 11 bit
		Temp &= ~0x01;
		break;
	default: // 12 bit
		break;
	}
	return (float)Temp/16.0f;
}

// Dallas/Maxim CRC-8, x^8 + x^5 + x^4 + 1, as the DS18B20 puts in
// the last byte of its scratchpad.
template <typename TTransport>
uint8_t ds18b20<TTransport>::crc8(const uint8_t *data, uint8_t len){
	uint8_t crc=0;
	for(uint8_t i=0;i<len;i++){
		uint8_t inbyte=data[i];
		for(uint8_t j=0;j<8;j++){
			uint8_t mix=(crc^inbyte)&0x01;
			crc>>=1;
			if(mix){
				crc^=0x8C;
			}
			inbyte>>=1;
		}
	}
	return crc;
}

template <typename TTransport>
bool ds18b20<TTransport>::newData(){
	return _newData;
}

template <typename TTransport>
void ds18b20<TTransport>::getTemp(float *temp){
	*temp=_temp;
	_newData=false;
}

} /* namespace DS18B20 */

#endif /* DS18B20_H_ */
