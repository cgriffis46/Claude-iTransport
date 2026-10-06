/**
  ******************************************************************************
  * @file     SHT31.h
  * @author   cgriffis46
  * @version  V1.0
  * @date     18/03/2024 13:49:10
  * @brief    Default under dev library file.
  ******************************************************************************
  *
  *  SHT31 temperature / humidity sensor, non-blocking state machine.
  *
  *  The bus is chosen by the template argument. SHT31<TTransport>
  *  inherits from TTransport, which must be an ISensorTransport
  *  (itransport/inc/ISensorTransport.h):
  *
  *      SHT31<Stm32HalI2CTransport>  sensor(param, &hi2c1, sht31_i2c_addr1, i2c1Mutex);
  *
  *  Everything after param goes to the transport's own constructor.
  *  This file includes no HAL and no RTOS header.
  *
  *  The SHT31 has no registers. It takes 16 bit commands and is read
  *  back as a plain run of bytes, so this driver uses the transport's
  *  writeBytes() and readBytes().
  *
  *  Call main(now) every pass of the loop. It never waits on the bus:
  *  each command and each read is one state that issues it and one
  *  state that waits for it to land. When a state has nothing to do it
  *  calls sleep(), an empty stub here. Override sleep() for an OS (see
  *  xSHT31.h) to give up the CPU for that long.
  *
  *  Nothing here talks to the chip directly any more. heater(),
  *  setHighAlert(), setLowAlert(), SetPeriodicMode(), SendBreak() and
  *  ForceMeasurement() each record a request and return at once; main()
  *  carries the requests out between measurements. If the chip is in
  *  periodic mode it is stopped first, as the datasheet asks, and
  *  started again afterwards.
  *
  *  The state bookkeeping, sleep() and the issue/wait helpers are
  *  inherited from SensorStateMachine (isensor/inc), shared with the
  *  other drivers.
*/

#ifndef SHT31_H
#define SHT31_H

#include <stdint.h>
#include <math.h>
#include <type_traits>
#include <utility>
#include "ISensorTransport.h"
#include "SensorStateMachine.h"

static const uint16_t SHT31_DEFAULT_ADDR = 0x44; /**< SHT31 Default Address */
static const uint16_t SHT31_DEFAULT_ADDR2 = 0x45; /**< SHT31 Default Address */

#define SHT31_MEAS_HIGHREP_STRETCH                                             \
  0x2C06 /**< Measurement High Repeatability with Clock Stretch Enabled */
#define SHT31_MEAS_MEDREP_STRETCH                                              \
  0x2C0D /**< Measurement Medium Repeatability with Clock Stretch Enabled */
#define SHT31_MEAS_LOWREP_STRETCH                                              \
  0x2C10 /**< Measurement Low Repeatability with Clock Stretch Enabled*/
#define SHT31_MEAS_HIGHREP                                                     \
  0x2400 /**< Measurement High Repeatability with Clock Stretch Disabled */
#define SHT31_MEAS_MEDREP                                                      \
  0x240B /**< Measurement Medium Repeatability with Clock Stretch Disabled */
#define SHT31_MEAS_LOWREP                                                      \
  0x2416 /**< Measurement Low Repeatability with Clock Stretch Disabled */
#define SHT31_READSTATUS 0xF32D   /**< Read Out of Status Register */
#define SHT31_CLEARSTATUS 0x3041  /**< Clear Status */
#define SHT31_SOFTRESET 0x30A2    /**< Soft Reset */
#define SHT31_HEATEREN 0x306D     /**< Heater Enable */
#define SHT31_HEATERDIS 0x3066    /**< Heater Disable */
#define SHT31_REG_HEATER_BIT 0x0d /**< Status Register Heater Bit */

#define SHT31_FETCH_DATA 0xE000
#define SHT31_BREAK 0x3093

#define SHT31_PERIODIC_05mps_HIGHREP 0x2032
#define SHT31_PERIODIC_05mps_MEDREP 0x2024
#define SHT31_PERIODIC_05mps_LOWREP 0x202F
#define SHT31_PERIODIC_1mps_HIGHREP 0x2130
#define SHT31_PERIODIC_1mps_MEDREP 0x2126
#define SHT31_PERIODIC_1mps_LOWREP 0x212D
#define SHT31_PERIODIC_2mps_HIGHREP 0x2236
#define SHT31_PERIODIC_2mps_MEDREP 0x2220
#define SHT31_PERIODIC_2mps_LOWREP 0x222B
#define SHT31_PERIODIC_4mps_HIGHREP 0x2334
#define SHT31_PERIODIC_4mps_MEDREP 0x2322
#define SHT31_PERIODIC_4mps_LOWREP 0x2329
#define SHT31_PERIODIC_10mps_HIGHREP 0x2737
#define SHT31_PERIODIC_10mps_MEDREP 0x2721
#define SHT31_PERIODIC_10mps_LOWREP 0x272A

#define SHT31_WRITE_HIGH_ALERT_SET 0x611D
#define SHT31_WRITE_HIGH_ALERT_CLEAR 0x6116
#define SHT31_WRITE_LOW_ALERT_CLEAR 0x610B
#define SHT31_WRITE_LOW_ALERT_SET 0x6100

typedef enum SHT31_addr_t{
	sht31_i2c_addr1=SHT31_DEFAULT_ADDR,
	sht31_i2c_addr2=SHT31_DEFAULT_ADDR2
}SHT31_addr_t;

// Every bus access is two states: one that issues it and a wait
// state that it lands in.
typedef enum sht31_state_t{
	sht31_init_state,					// check a device answers
	sht31_reset_state,					// issue the soft reset
	sht31_wait_reset_state,
	sht31_reset_settle_state,			// pause while the reset takes effect
	sht31_clearstatus_state,			// issue the clear status command
	sht31_wait_clearstatus_state,
	sht31_clearstatus_settle_state,		// pause while the status register clears
	sht31_status_cmd_state,				// issue the read status command
	sht31_wait_status_cmd_state,
	sht31_read_status_state,			// issue the 3 byte read
	sht31_wait_status_state,
	sht31_done_state,					// idle, chip measuring only when asked. requests are carried out here
	sht31_sleeping_state,				// idle, chip in periodic mode
	sht31_break_state,					// issue the break that stops periodic mode
	sht31_wait_break_state,
	sht31_break_settle_state,			// pause before the next command
	sht31_heater_state,					// issue heater on or off
	sht31_wait_heater_state,
	sht31_high_alert_set_state,
	sht31_wait_high_alert_set_state,
	sht31_high_alert_clear_state,
	sht31_wait_high_alert_clear_state,
	sht31_low_alert_set_state,
	sht31_wait_low_alert_set_state,
	sht31_low_alert_clear_state,
	sht31_wait_low_alert_clear_state,
	sht31_set_periodic_mode_state,		// issue the periodic mode command
	sht31_wait_set_periodic_mode_state,
	sht31_force_measurement_state,		// issue a single measurement
	sht31_wait_force_measurement_state,
	sht31_measuring_state,				// conversion time
	sht31_read_measurement_state,		// issue the 6 byte read
	sht31_wait_measurement_state,
	sht31_fetch_cmd_state,				// periodic mode: issue the fetch data command
	sht31_wait_fetch_cmd_state,
	sht31_fetch_read_state,				// issue the 6 byte read
	sht31_wait_fetch_state,
	sht31_error_state					// pause, then start again from sht31_init_state
}sht31_state_t;

// All times in ms.
const uint32_t sht31_bus_timeout_ms = 100;		// one bus operation, to issue or to land
const uint32_t sht31_reset_ms = 100;			// after the soft reset
const uint32_t sht31_clearstatus_ms = 100;		// after clearing the status register
const uint32_t sht31_break_ms = 2;				// after a break, before the next command
const uint32_t sht31_conversion_ms = 20;		// a single high repeatability measurement takes 15.5 at most
const uint32_t sht31_fetch_period_ms = 2000;	// between reads in periodic mode
const uint32_t sht31_request_poll_ms = 100;		// longest sleep while a request could arrive
const uint32_t sht31_error_backoff_ms = 100;	// in sht31_error_state before starting again

struct SHT31_Alert_t{
  float SetTemp, ClearTemp;
  float SetHumidity, ClearHumidity;
};

typedef enum {
  C = 0,
  F = 1
}temp_unit;

typedef enum{
  _05mps_high_Res = 0,
  _05_med_Res = 1,
  _05_low_Res = 2,
  _1mps_high_Res = 3,
  _1mps_med_Res = 4,
  _1mps_low_Res = 5,
  _2mps_high_Res = 6,
  _2mps_med_Res = 7,
  _2mps_low_Res = 8,
  _4mps_high_Res = 9,
  _4mps_med_Res = 10,
  _4mps_low_Res = 11,
  _10mps_high_Res = 12,
  _10mps_med_Res = 13,
  _10mps_low_Res = 14,
}SHT31_Sample_Rate_t;

// The bus handle and device address are no longer here. They belong
// to the transport and go to its constructor.
typedef struct SHT31_param_t{
	bool periodic_mode;
	bool heater_enable;
	SHT31_Sample_Rate_t sample_rate;
}SHT31_param_t;

template <typename TTransport>
class SHT31 : public SensorStateMachine<TTransport, sht31_state_t> {
	static_assert(std::is_base_of<ISensorTransport, TTransport>::value,
			"SHT31<TTransport>: TTransport must derive from ISensorTransport");
	typedef SensorStateMachine<TTransport, sht31_state_t> base;
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
  explicit SHT31(const SHT31_param_t &sht31_param, TArgs&&... transportArgs);
  virtual ~SHT31() {}

  void main(uint32_t nowMs);
  bool newData();
  bool GetTemperature(float *t);	// degrees C. returns whether the value is new
  bool GetHumidity(float *h);		// %RH. returns whether the value is new

  // Each of these records a request and returns at once. main()
  // carries it out the next time the driver is between measurements.
  bool ForceMeasurement(); // start a forced measurement
  void heater(bool h);
  void SetPeriodicMode(SHT31_Sample_Rate_t mps);	// switch to periodic mode at this rate
  void SendBreak();									// leave periodic mode
  void setHighAlert(const SHT31_Alert_t* alert);
  void setLowAlert(const SHT31_Alert_t* alert);

  uint16_t readStatus(void);	// the status register as last read: at start up, and after each heater change
  bool isHeaterEnabled();		// from that same status register
  void ReadLowAlert(SHT31_Alert_t* alert);
  void ReadHighAlert(SHT31_Alert_t* alert);
  bool HighTempActive();
  bool LowTempActive();
  bool HighHumidityActive();
  bool LowHumidityActive();
private:
  bool _newdata;
  bool _force_measurement;
  bool _update_periodic_mode;
  bool _update_heater;
  bool _update_high_alert;
  bool _update_low_alert;
  bool _break_requested;
  bool _heater;
  bool _chip_periodic;		// the chip itself is in periodic mode right now
  /**
   * Placeholder to track humidity internally.
   */
  float humidity;
  /**
   * Placeholder to track temperature internally.
   */
  float temp;

  temp_unit unit = C;

  bool _PeriodicMode;		// periodic mode is wanted
  SHT31_Sample_Rate_t _mps;
  SHT31_Alert_t HighAlert;
  SHT31_Alert_t LowAlert;

  bool TempHighAlert, TempLowAlert, HumidityHighAlert, HumidityLowAlert;

  uint16_t _status;

  // The transport fills this in after main() has returned, so it
  // has to be a member and not a local.
  uint8_t dataReg[6];

  bool requestPending() const;
  bool writeCommand(uint16_t cmd);
  bool writeAlert(uint16_t cmd, float t, float h);
  uint16_t periodicCommand(SHT31_Sample_Rate_t mps) const;
  bool convert();
  static uint8_t crc8(const uint8_t *data, int len);
};

/*
 * Templates have to be visible wherever they are used, so the code
 * that was in SHT31.cpp lives below.
 */

/*!
 * @brief  SHT31 constructor
 * @param  sht31_param settings
 * @param  transportArgs passed on to the transport
 */
template <typename TTransport>
template <typename... TArgs>
SHT31<TTransport>::SHT31(const SHT31_param_t &sht31_param, TArgs&&... transportArgs)
	: base(sht31_init_state, sht31_error_state, sht31_bus_timeout_ms, std::forward<TArgs>(transportArgs)...) {
  this->_mps=sht31_param.sample_rate;
  _PeriodicMode=sht31_param.periodic_mode;
  _heater=sht31_param.heater_enable;
  humidity = NAN;
  temp = NAN;
  TempHighAlert = false; TempLowAlert = false; HumidityHighAlert = false; HumidityLowAlert = false;
  // No limits until setHighAlert()/setLowAlert(): nothing compares true against NAN.
  HighAlert.SetTemp=NAN; HighAlert.ClearTemp=NAN; HighAlert.SetHumidity=NAN; HighAlert.ClearHumidity=NAN;
  LowAlert=HighAlert;
  _newdata=false;
  _force_measurement=false;
  _update_periodic_mode=false;
  _update_heater=false;
  _update_high_alert=false;
  _update_low_alert=false;
  _break_requested=false;
  _chip_periodic=false;
  _status=0;
}

template <typename TTransport>
void SHT31<TTransport>::main(uint32_t nowMs){
	switch(_state){
	case sht31_init_state:
		if(this->checkDevice()){ // if device exists, go to reset.
			enter(sht31_reset_state,nowMs);
		}
		else { // if device doesn't exist go to error state.
			fail(nowMs);
		}
		break;
	case sht31_reset_state:
		issued(writeCommand(SHT31_SOFTRESET),sht31_wait_reset_state,nowMs);
		break;
	case sht31_wait_reset_state:
		if(landed(nowMs)){
			// The reset leaves the chip idle with the heater off.
			_chip_periodic=false;
			_update_heater=_heater;
			if(!_PeriodicMode){
				_force_measurement=true; // perform a forced measurement so there is a reading from the start.
			}
			enter(sht31_reset_settle_state,nowMs);
		}
		break;
	case sht31_reset_settle_state:
		if(elapsed(nowMs,sht31_reset_ms)){// wait 100ms for device to reset. then begin init.
			enter(sht31_clearstatus_state,nowMs);
		} else {
			sleepRemaining(nowMs,sht31_reset_ms);
		}
		break;
	case sht31_clearstatus_state:
		issued(writeCommand(SHT31_CLEARSTATUS),sht31_wait_clearstatus_state,nowMs); // clear status register
		break;
	case sht31_wait_clearstatus_state:
		if(landed(nowMs)){
			enter(sht31_clearstatus_settle_state,nowMs);
		}
		break;
	case sht31_clearstatus_settle_state:
		if(elapsed(nowMs,sht31_clearstatus_ms)){ // wait for status register to clear
			enter(sht31_status_cmd_state,nowMs);
		} else {
			sleepRemaining(nowMs,sht31_clearstatus_ms);
		}
		break;
	case sht31_status_cmd_state:
		issued(writeCommand(SHT31_READSTATUS),sht31_wait_status_cmd_state,nowMs);
		break;
	case sht31_wait_status_cmd_state:
		if(landed(nowMs)){
			enter(sht31_read_status_state,nowMs);
		}
		break;
	case sht31_read_status_state:
		issued(this->readBytes(dataReg,3),sht31_wait_status_state,nowMs);
		break;
	case sht31_wait_status_state:
		if(landed(nowMs)){
			if(crc8(dataReg,2)!=dataReg[2]){
				fail(nowMs);
			} else {
				_status = dataReg[0];
				_status <<= 8;
				_status |= dataReg[1];
				enter(sht31_done_state,nowMs);
			}
		}
		break;
	case sht31_done_state:
		// The chip is idle. Carry out whatever has been asked for, one
		// thing per pass, then go back to periodic mode if that is wanted.
		_break_requested=false; // already out of periodic mode
		if(_update_heater){
			enter(sht31_heater_state,nowMs);
		} else if(_update_high_alert){
			enter(sht31_high_alert_set_state,nowMs);
		} else if(_update_low_alert){
			enter(sht31_low_alert_set_state,nowMs);
		} else if(_force_measurement){
			enter(sht31_force_measurement_state,nowMs);
		} else if(_PeriodicMode){ // if the sensor should be in periodic mode, set it up.
			enter(sht31_set_periodic_mode_state,nowMs);
		} else {
			sleep(sht31_request_poll_ms);
		}
		break;
	case sht31_sleeping_state: // should be in periodic mode.
		if(requestPending()){ // break from periodic mode before any other command
			enter(sht31_break_state,nowMs);
		}
		else if(elapsed(nowMs,sht31_fetch_period_ms)){
			enter(sht31_fetch_cmd_state,nowMs);
		} else {
			// No longer than sht31_request_poll_ms at a time, so a
			// request is seen without waiting out the whole period.
			uint32_t left=sht31_fetch_period_ms-(nowMs-last_update);
			sleep(left<sht31_request_poll_ms?left:sht31_request_poll_ms);
		}
		break;
	case sht31_break_state:
		issued(writeCommand(SHT31_BREAK),sht31_wait_break_state,nowMs);
		break;
	case sht31_wait_break_state:
		if(landed(nowMs)){
			_chip_periodic=false;
			enter(sht31_break_settle_state,nowMs);
		}
		break;
	case sht31_break_settle_state: // wait 2ms after a break before the next command
		if(elapsed(nowMs,sht31_break_ms)){
			enter(sht31_done_state,nowMs);
		} else {
			sleepRemaining(nowMs,sht31_break_ms);
		}
		break;
	case sht31_heater_state:
		_update_heater=false;
		issued(writeCommand(_heater?SHT31_HEATEREN:SHT31_HEATERDIS),sht31_wait_heater_state,nowMs);
		break;
	case sht31_wait_heater_state:
		if(landed(nowMs)){
			enter(sht31_status_cmd_state,nowMs); // read the status back, so isHeaterEnabled() is the chip's answer
		}
		break;
	case sht31_high_alert_set_state:
		_update_high_alert=false;
		issued(writeAlert(SHT31_WRITE_HIGH_ALERT_SET,HighAlert.SetTemp,HighAlert.SetHumidity),sht31_wait_high_alert_set_state,nowMs);
		break;
	case sht31_wait_high_alert_set_state:
		if(landed(nowMs)){
			enter(sht31_high_alert_clear_state,nowMs);
		}
		break;
	case sht31_high_alert_clear_state:
		issued(writeAlert(SHT31_WRITE_HIGH_ALERT_CLEAR,HighAlert.ClearTemp,HighAlert.ClearHumidity),sht31_wait_high_alert_clear_state,nowMs);
		break;
	case sht31_wait_high_alert_clear_state:
		if(landed(nowMs)){
			enter(sht31_done_state,nowMs);
		}
		break;
	case sht31_low_alert_set_state:
		_update_low_alert=false;
		issued(writeAlert(SHT31_WRITE_LOW_ALERT_SET,LowAlert.SetTemp,LowAlert.SetHumidity),sht31_wait_low_alert_set_state,nowMs);
		break;
	case sht31_wait_low_alert_set_state:
		if(landed(nowMs)){
			enter(sht31_low_alert_clear_state,nowMs);
		}
		break;
	case sht31_low_alert_clear_state:
		issued(writeAlert(SHT31_WRITE_LOW_ALERT_CLEAR,LowAlert.ClearTemp,LowAlert.ClearHumidity),sht31_wait_low_alert_clear_state,nowMs);
		break;
	case sht31_wait_low_alert_clear_state:
		if(landed(nowMs)){
			enter(sht31_done_state,nowMs);
		}
		break;
	case sht31_set_periodic_mode_state:
		_update_periodic_mode=false;
		issued(writeCommand(periodicCommand(_mps)),sht31_wait_set_periodic_mode_state,nowMs);
		break;
	case sht31_wait_set_periodic_mode_state:
		if(landed(nowMs)){
			_chip_periodic=true;
			enter(sht31_sleeping_state,nowMs);
		}
		break;
	case sht31_force_measurement_state:
		_force_measurement=false;
		issued(writeCommand(SHT31_MEAS_HIGHREP),sht31_wait_force_measurement_state,nowMs);
		break;
	case sht31_wait_force_measurement_state:
		if(landed(nowMs)){
			enter(sht31_measuring_state,nowMs);
		}
		break;
	case sht31_measuring_state:
		if(elapsed(nowMs,sht31_conversion_ms)){
			enter(sht31_read_measurement_state,nowMs);
		} else {
			sleepRemaining(nowMs,sht31_conversion_ms);
		}
		break;
	case sht31_read_measurement_state:
		// A single measurement is read straight back. The fetch data
		// command is for periodic mode only.
		issued(this->readBytes(dataReg,6),sht31_wait_measurement_state,nowMs);
		break;
	case sht31_wait_measurement_state:
		if(landed(nowMs)){
			if(convert()){
				enter(sht31_done_state,nowMs);
			} else {
				fail(nowMs);
			}
		}
		break;
	case sht31_fetch_cmd_state:
		issued(writeCommand(SHT31_FETCH_DATA),sht31_wait_fetch_cmd_state,nowMs);
		break;
	case sht31_wait_fetch_cmd_state:
		if(landed(nowMs)){
			enter(sht31_fetch_read_state,nowMs);
		}
		break;
	case sht31_fetch_read_state:
		issued(this->readBytes(dataReg,6),sht31_wait_fetch_state,nowMs);
		break;
	case sht31_wait_fetch_state:
		// The chip does not acknowledge the read when it has nothing
		// new, so an error here only means try again next time. A chip
		// that has gone away fails the fetch command before this.
		if(finished(nowMs)){
			if(this->lastOpFailed()){
				enter(sht31_sleeping_state,nowMs);
			} else if(convert()){
				enter(sht31_sleeping_state,nowMs);
			} else {
				fail(nowMs);
			}
		}
		break;
	case sht31_error_state:
		if(errorCleared(nowMs,sht31_error_backoff_ms)){
			enter(sht31_init_state,nowMs);
		}
		break;
	default:
		enter(sht31_init_state,nowMs);
		break;
	}

}

template <typename TTransport>
bool SHT31<TTransport>::requestPending() const{
	return _force_measurement||_update_heater||_update_high_alert||_update_low_alert
			||_update_periodic_mode||_break_requested;
}

/**
 * Internal function to perform and I2C write.
 *
 * @param cmd   The 16-bit command ID to send.
 */
template <typename TTransport>
bool SHT31<TTransport>::writeCommand(uint16_t command) {

	uint8_t cmd[2];

  cmd[0] = command >> 8;
  cmd[1] = command & 0xFF;

  return this->writeBytes(cmd,2); // the transport takes its own copy
}

// One alert limit: the command, the limit packed as the top 7 bits of
// humidity over the top 9 bits of temperature, and its CRC.
template <typename TTransport>
bool SHT31<TTransport>::writeAlert(uint16_t command, float t, float h){
  uint16_t Temp, Humidity;
  uint16_t rht;

  uint8_t cmd[5];

  switch (this->unit) {
    case F:
    {
      Temp = static_cast<uint16_t>(((t+49)/315)*65535.0f);
      break;
    }
    default:
    {
      Temp = static_cast<uint16_t>(((t+45)/175)*65535.0f);
      break;
    }
  }

  Humidity = static_cast<uint16_t>((h*65535.0f)/100);

  rht = (Humidity&0xFE00);
  rht |= (Temp>>7);

  cmd[0] = command >> 8;
  cmd[1] = command & 0xFF;
  cmd[2] = (uint8_t)(rht>>8);
  cmd[3] = (uint8_t)(rht&0xFF);
  cmd[4] = crc8(&cmd[2],2);
  return this->writeBytes(cmd,5);
}

template <typename TTransport>
uint16_t SHT31<TTransport>::periodicCommand(SHT31_Sample_Rate_t mps) const{
  switch (mps) {
    case _05mps_high_Res: return SHT31_PERIODIC_05mps_HIGHREP;
    case _05_med_Res: return SHT31_PERIODIC_05mps_MEDREP;
    case _05_low_Res: return SHT31_PERIODIC_05mps_LOWREP;
    case _1mps_high_Res: return SHT31_PERIODIC_1mps_HIGHREP;
    case _1mps_med_Res: return SHT31_PERIODIC_1mps_MEDREP;
    case _1mps_low_Res: return SHT31_PERIODIC_1mps_LOWREP;
    case _2mps_high_Res: return SHT31_PERIODIC_2mps_HIGHREP;
    case _2mps_med_Res: return SHT31_PERIODIC_2mps_MEDREP;
    case _2mps_low_Res: return SHT31_PERIODIC_2mps_LOWREP;
    case _4mps_high_Res: return SHT31_PERIODIC_4mps_HIGHREP;
    case _4mps_med_Res: return SHT31_PERIODIC_4mps_MEDREP;
    case _4mps_low_Res: return SHT31_PERIODIC_4mps_LOWREP;
    case _10mps_high_Res: return SHT31_PERIODIC_10mps_HIGHREP;
    case _10mps_med_Res: return SHT31_PERIODIC_10mps_MEDREP;
    case _10mps_low_Res: return SHT31_PERIODIC_10mps_LOWREP;
    default: return SHT31_PERIODIC_1mps_LOWREP;
  }
}

// Turns the 6 bytes in dataReg into temp and humidity. false if either
// CRC is wrong.
template <typename TTransport>
bool SHT31<TTransport>::convert(){
    if (dataReg[2] != crc8(dataReg, 2) ||
      dataReg[5] != crc8(dataReg + 3, 2))
    return false;

  int32_t stemp = (int32_t)(((uint32_t)dataReg[0] << 8) + dataReg[1]);
  // simplified (65536 instead of 65535) integer version of:
  // temp = (stemp * 175.0f) / 65535.0f - 45.0f;
  stemp = ((4375 * stemp) >> 14) - 4500;
  this->temp = (float)stemp / 100.0f;
  uint32_t shum = ((uint32_t)dataReg[3] << 8) + dataReg[4];
  // simplified (65536 instead of 65535) integer version of:
  // humidity = (shum * 100.0f) / 65535.0f;
  shum = (625 * shum) >> 12;
  this->humidity = (float)shum / 100.0f;

  if(this->humidity>=this->HighAlert.SetHumidity){this->HumidityHighAlert = true;}
  if(this->humidity<=this->HighAlert.ClearHumidity){this->HumidityHighAlert = false;}
  if(this->temp>=this->HighAlert.SetTemp){this->TempHighAlert = true;}
  if(this->temp<=this->HighAlert.ClearTemp){this->TempHighAlert = false;}

  if(this->humidity<=this->LowAlert.SetHumidity){this->HumidityLowAlert = true;}
  if(this->humidity>=this->LowAlert.ClearHumidity){this->HumidityLowAlert = false;}
  if(this->temp<=this->LowAlert.SetTemp){this->TempLowAlert = true;}
  if(this->temp>=this->LowAlert.ClearTemp){this->TempLowAlert = false;}

  _newdata=true;
  return true;
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
uint8_t SHT31<TTransport>::crc8(const uint8_t *data, int len) {
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

/**
 * Gets the status register contents as last read from the chip.
 *
 * @return The 16-bit status register.
 */
template <typename TTransport>
uint16_t SHT31<TTransport>::readStatus(void) {
  return _status;
}

/**
 * Enables or disabled the heating element.
 *
 * @param h True to enable the heater, False to disable it.
 */
template <typename TTransport>
void SHT31<TTransport>::heater(bool h) {
  _heater=h;
  _update_heater=true;
}

/*!
 *  @brief  Return sensor heater state
 *  @return heater state (TRUE = enabled, FALSE = disabled)
 */
template <typename TTransport>
bool SHT31<TTransport>::isHeaterEnabled() {
  return ((_status>>SHT31_REG_HEATER_BIT)&0x01)!=0;
}

template <typename TTransport>
void SHT31<TTransport>::setHighAlert(const SHT31_Alert_t* alert){
  this->HighAlert=*alert;
  _update_high_alert=true;
}

template <typename TTransport>
void SHT31<TTransport>::setLowAlert(const SHT31_Alert_t* alert){
  this->LowAlert=*alert;
  _update_low_alert=true;
}

template <typename TTransport>
void SHT31<TTransport>::ReadLowAlert(SHT31_Alert_t* alert){
  *alert=this->LowAlert;
}

template <typename TTransport>
void SHT31<TTransport>::ReadHighAlert(SHT31_Alert_t* alert){
  *alert=this->HighAlert;
}

template <typename TTransport>
bool SHT31<TTransport>::HighTempActive(){
  return TempHighAlert;
}

template <typename TTransport>
bool SHT31<TTransport>::LowTempActive(){
  return TempLowAlert;
}

template <typename TTransport>
bool SHT31<TTransport>::HighHumidityActive(){
  return HumidityHighAlert;
}

template <typename TTransport>
bool SHT31<TTransport>::LowHumidityActive(){
  return HumidityLowAlert;
}

/*
 * The datasheet recommends sending a Break command to the SHT31
 * before issuing commands.
 * Especially if the sensor is in Periodic mode or in a Measurement.
 * main() does that itself before every request. Calling this leaves
 * periodic mode for good, until SetPeriodicMode() is called again.
 */
template <typename TTransport>
void SHT31<TTransport>::SendBreak(){
	_PeriodicMode=false;
	_break_requested=true;
}

/*
 * If the SHT31 is in Periodic mode, a Break is issued first.
 * When the measurement completes the SHT31 goes back to Periodic mode
 * if that is how it is set up, and otherwise waits to be asked again.
 */
template <typename TTransport>
bool SHT31<TTransport>::ForceMeasurement(){
	_force_measurement=true;
	return true;
}

template <typename TTransport>
void SHT31<TTransport>::SetPeriodicMode(SHT31_Sample_Rate_t mps){
	this->_mps=mps;
	_PeriodicMode=true;
	_update_periodic_mode=true;
}

template <typename TTransport>
bool SHT31<TTransport>::GetTemperature(float *t){
	bool old = _newdata;
	_newdata=false;
	*t=this->temp;
	return old;
}

template <typename TTransport>
bool SHT31<TTransport>::GetHumidity(float *h){
	bool old = _newdata;
	_newdata=false;
	*h=this->humidity;
	return old;
}

template <typename TTransport>
bool SHT31<TTransport>::newData(){
	return _newdata;
}

#endif
