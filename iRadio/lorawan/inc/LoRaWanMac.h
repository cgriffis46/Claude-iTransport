/*
 * LoRaWanMac.h
 *
 *  A LoRaWAN 1.0.4 Class A end device: OTAA join, unconfirmed and
 *  confirmed uplinks, downlinks in RX1 and RX2, the MAC commands, and ADR
 *  with the device side backoff. Over any iLoRaRadio (rfm95), with the
 *  region's rules from a Region (RegionUS915), and the session kept in an
 *  iSessionStore the application provides (flash, EEPROM, RTC backup
 *  registers, a LittleFS file). Everything is injected and outlives the
 *  Mac. No heap.
 *
 *      sim/flash store, RegionUS915 region(2), rfm95<...> radio(...);
 *      lorawan::Mac mac(radio, region, store, param);
 *      mac.begin(HAL_GetTick());       // loads the session, if any
 *      if (!mac.joined()) mac.join();
 *      for (;;) {
 *          mac.main(HAL_GetTick());    // runs the radio too
 *          lorawan::MacEvent e; uint8_t buf[242];
 *          while (mac.takeEvent(&e, buf, sizeof buf)) ...
 *          if (mac.ready() && timeToSend) mac.send(port, data, len, false);
 *      }
 *
 *  main() drives the radio's main() as well: one thread, nothing else
 *  calling the radio. Give the plain rfm95 (its sleep() does nothing); the
 *  RTOS loop (xLoRaWanMac.h) sleeps for the MAC, as long as sleepHintMs().
 *
 *  Timing. The receive windows are timed from the TxDone event's time
 *  stamp in the radio's ticks (an iClock on the radio, or ms), so wire
 *  DIO0 to an interrupt that calls the radio's onDio0(). Each window opens
 *  early and listens long enough for rxErrorMs of clock error either way
 *  and minRxSymbols of preamble (LoRaMac-node's formula). Longer waits
 *  (backoff, retransmission, duty cycle) are in the ms given to main().
 *
 *  The session. DevNonce is a counter (1.0.4): it is saved before each
 *  join request goes out, and if the save fails the request is not sent.
 *  FCntUp is saved ahead in steps of saveEvery (a reset skips at most that
 *  many, never reuses one). FCntDown is saved with it, so after a reset
 *  the last few downlinks could be accepted again (the gap is at most the
 *  downlinks since the last save). The record carries the DevEUI and a
 *  CRC; a record for another DevEUI, a different version or a bad CRC is
 *  ignored (DevNonce starts at 0 again).
 *
 *  Not here: Class B and C, ABP, LoRaWAN 1.1, rejoin, FSK, NewChannelReq
 *  and DlChannelReq (not used in US915), TxParamSetupReq (AS923 only).
 */

#ifndef LORAWANMAC_H_
#define LORAWANMAC_H_

#include <stdint.h>
#include "iLoRaRadio.h"
#include "LoRaWanFrame.h"
#include "LoRaWanRegion.h"

namespace lorawan {

// Where the session lives. Both calls are made from main()'s thread and
// may block briefly (a flash write).
class iSessionStore {
public:
	virtual ~iSessionStore() {}
	// Fill p with the last record saved (len bytes); false if there is none.
	virtual bool load(uint8_t* p, uint16_t len) = 0;
	virtual bool save(const uint8_t* p, uint16_t len) = 0;
};

struct MacParam {
	uint8_t  devEui[8];        // most significant byte first, as TTN's console prints them
	uint8_t  joinEui[8];       // (AppEUI)
	uint8_t  appKey[16];
	uint8_t  dataRate;         // uplink data rate to start with, and to keep with ADR off
	bool     adr;              // let the network set data rate, power and NbTrans
	int8_t   antennaGainDb;    // subtracted from the region's EIRP (LoRaMac-node assumes 2.15 dBi)
	uint8_t  confirmedTries;   // transmissions of a confirmed uplink at least (more if NbTrans says)
	uint16_t saveEvery;        // FCntUp step between saves of the session
	uint32_t randomSeed;       // channel choice and backoff jitter (a chip ID, an ADC's noise)
	uint8_t  rxErrorMs;        // clock error allowed at each receive window, either way
	uint8_t  minRxSymbols;     // preamble symbols the radio needs to lock on
	uint8_t  radioWakeMs;      // from receive() to listening (register writes, PLL)
	uint8_t  battery;          // DevStatusAns: 0 external power, 1-254 level, 255 unknown
};

MacParam defaultMacParam();

enum MacEventKind : uint8_t {
	mac_ev_joined,           // the join accept arrived: the device has a session
	mac_ev_join_failed,      // maxJoinTries without an answer (join() again to keep trying)
	mac_ev_tx_done,          // an uplink is finished (ack: a confirmed one was acknowledged)
	mac_ev_downlink,         // application data (port 1-223)
	mac_ev_link_check,       // LinkCheckAns: margin (dB above the demodulation floor), gateways
	mac_ev_device_time,      // DeviceTimeAns: GPS time at the end of the uplink that asked
	mac_ev_fault             // the store or the radio failed; code says which
};

enum MacFault : uint8_t {
	mac_fault_store = 1,     // a save failed: no join request was sent
	mac_fault_radio,         // the radio reported a fault or refused a request
	mac_fault_too_long,      // the payload no longer fits the data rate: the uplink was dropped
	mac_fault_nonce_spent    // DevNonce reached 65535: the device needs new keys (1.0.4)
};

struct MacEvent {
	MacEventKind kind;
	bool     ack;            // tx_done: acknowledged (confirmed uplinks)
	bool     fPending;       // downlink: the network has more to send
	uint8_t  port;           // downlink
	uint8_t  len;            // downlink: bytes in the payload
	int16_t  rssiDbm;        // downlink, link_check
	int8_t   snrDb;
	uint8_t  margin, gateways;            // link_check
	uint32_t gpsSeconds; uint8_t gpsFraction;   // device_time (1/256 s)
	uint32_t ticks;          // device_time: the radio tick it refers to (the uplink's end)
	uint8_t  fault;          // MacFault
};

class Mac {
public:
	static const uint8_t  kMaxPayload = 242;   // the largest N of any data rate
	static const uint16_t kSessionBytes = 91;
	static const uint8_t  kEvents = 4;
	static const uint8_t  kAdrAckLimit = 64, kAdrAckDelay = 32;
	static const uint32_t kReceiveDelay1Ms = 1000, kJoinAcceptDelay1Ms = 5000;   // RX2 is a second later

	Mac(lora::iLoRaRadio& radio, Region& region, iSessionStore& store, const MacParam& param);

	// Loads the session (joined or not). Call once, before the rest.
	void begin(uint32_t nowMs);
	void main(uint32_t nowMs);

	// Requests. False if busy (ready() is false) or invalid.
	bool join(uint16_t maxTries = 0);   // 0: until it succeeds
	bool send(uint8_t port, const uint8_t* data, uint8_t len, bool confirmed);
	// Ask with the next uplink: LinkCheckReq, DeviceTimeReq.
	void requestLinkCheck() { _wantLinkCheck = true; }
	void requestDeviceTime() { _wantDeviceTime = true; }

	bool joined() const { return _joined; }
	bool ready() const { return _st == St::Idle; }
	// MAC answers wait for the next uplink (send one, empty if need be: port 1, len 0).
	bool macPending() const { return _ansLen > 0 || _stickyLen > 0; }
	// Most application bytes the next uplink can carry now.
	uint8_t maxPayloadNow() const;

	bool takeEvent(MacEvent* e, uint8_t* buf, uint8_t cap);

	// How long main() may sleep (ms): until the next timer, at most 100.
	uint32_t sleepHintMs(uint32_t nowMs) const;

	// State, for tests and diagnostics.
	uint32_t devAddr() const { return _devAddr; }
	uint32_t fCntUp() const { return _fCntUp; }
	uint32_t fCntDown() const { return _fCntDown; }
	uint16_t devNonce() const { return _devNonce; }
	const AdrState& adrState() const { return _adr; }
	uint8_t rx1DrOffset() const { return _rx1DrOffset; }
	uint8_t rx2Dr() const { return _rx2Dr; }
	uint32_t rx2Freq() const { return _rx2Freq; }
	uint8_t rx1DelayS() const { return _rx1Delay; }
	uint8_t maxDutyCycle() const { return _maxDutyCycle; }
	uint32_t adrAckCounter() const { return _adrAckCnt; }
	uint32_t eventsDropped() const { return _dropped; }

	// The RX window timing LoRaMac-node uses: how many symbols to listen for,
	// and when to open relative to the nominal start (ms, negative = early).
	static void rxWindow(uint32_t symbolUs, uint8_t minSymbols, uint32_t errorMs, uint32_t wakeMs,
	                     uint16_t* symbols, int32_t* offsetMs);

private:
	enum class St : uint8_t { Idle, Wait, TxStart, TxWait, Rx1Wait, Rx1, Rx2Wait, Rx2 };
	enum class Job : uint8_t { None, Join, Data };

	uint32_t rnd();
	uint32_t msToTicks(uint32_t ms) const;
	bool after(uint32_t t, uint32_t now) const { return (int32_t)(now - t) >= 0; }

	bool saveSession();
	void encode(uint8_t* p) const;
	bool decode(const uint8_t* p);
	void resetSessionState();

	void startTx(uint32_t nowMs);
	bool buildFrame();
	void planWindows(uint32_t txEndTicks);
	bool openWindow(uint8_t which, uint32_t nowMs);
	bool handleRx(const uint8_t* buf, uint8_t len, int16_t rssi, int8_t snr);
	bool handleJoinAccept(uint8_t* buf, uint8_t len);
	bool handleData(uint8_t* buf, uint8_t len, int16_t rssi, int8_t snr);
	void handleCommands(const uint8_t* p, uint8_t len, int8_t snr);
	void finishCycle(uint32_t nowMs);
	void adrNext();
	void addAnswer(uint8_t cid, const uint8_t* v, uint8_t n);
	void addSticky(uint8_t cid, const uint8_t* v, uint8_t n);
	void push(const MacEvent& e);
	void pushSimple(MacEventKind k, uint8_t fault = 0);

	lora::iLoRaRadio& _radio;
	Region& _region;
	iSessionStore& _store;
	MacParam _p;
	uint32_t _rng;

	// The session (saved).
	bool     _joined = false;
	uint16_t _devNonce = 0;
	bool     _haveJoinNonce = false;
	uint32_t _joinNonce = 0;
	uint32_t _devAddr = 0;
	uint8_t  _nwkSKey[16], _appSKey[16];
	uint32_t _fCntUp = 0;            // the next new uplink's
	uint32_t _fCntUpSaved = 0;       // saved ceiling: _fCntUp stays below it
	uint32_t _fCntDown = 0;          // the last accepted
	bool     _downSeen = false;
	uint8_t  _rx1DrOffset = 0, _rx2Dr = 0, _rx1Delay = 1, _maxDutyCycle = 0;
	uint32_t _rx2Freq = 0;
	AdrState _adr;

	// The job in progress.
	St       _st = St::Idle;
	Job      _job = Job::None;
	uint32_t _waitUntilMs = 0;       // Wait: when to start the next transmission
	uint32_t _stateMs = 0;           // when the state began (safety timeouts)
	uint16_t _joinTries = 0, _joinMax = 0;
	uint16_t _joinNonceUsed = 0;
	uint32_t _joinStartMs = 0;
	uint8_t  _tries = 0, _triesMax = 1;
	bool     _confirmed = false, _acked = false, _gotDownlink = false, _fPendingSeen = false;
	uint8_t  _port = 0, _len = 0;
	uint8_t  _data[kMaxPayload];
	uint32_t _frameFCnt = 0;
	uint8_t  _frame[255];
	uint8_t  _frameLen = 0;
	uint8_t  _txDr = 0, _txChannel = 0;
	uint32_t _txFreq = 0, _txEnd = 0, _txToaMs = 0;
	uint32_t _txEndMs = 0;           // the TxDone, in main()'s ms: time-off counts from here
	uint32_t _rxOpen[2], _rxFreq[2];
	uint8_t  _rxDr[2];
	uint16_t _rxSymbols[2];
	uint32_t _nextTxMs = 0;          // duty cycle: no transmission before this
	bool     _dutyLimit = false;
	bool     _ackDownlink = false;   // a confirmed downlink to acknowledge
	bool     _wantLinkCheck = false, _wantDeviceTime = false;
	bool     _sentLinkCheck = false, _sentDeviceTime = false;

	// ADR.
	uint32_t _adrAckCnt = 0;
	bool     _adrAckReq = false;

	// MAC answers for the next uplink.
	uint8_t  _ans[kMaxFOpts], _ansLen = 0;
	uint8_t  _sticky[kMaxFOpts], _stickyLen = 0;   // repeated until a downlink arrives
	uint8_t  _fopts[kMaxFOpts], _foptsLen = 0;

	// Events.
	MacEvent _ev[kEvents];
	uint8_t  _evHead = 0, _evCount = 0;
	uint8_t  _down[kMaxPayload];
	bool     _downHeld = false;      // _down belongs to a downlink event not yet taken
	uint32_t _dropped = 0;
	uint8_t  _rxBuf[255];
};

} // namespace lorawan

#endif /* LORAWANMAC_H_ */
