/*
 * LoRaWanFrame.h
 *
 *  LoRaWAN 1.0.x frames, the end device's side: the join request, the join
 *  accept (decrypted and checked), the session keys, and data frames
 *  (FCtrl, FCnt, FOpts, FPort, the encrypted FRMPayload and the MIC). Pure
 *  logic: no radio, no time, no heap.
 *
 *  Layouts (all multi-byte fields little-endian on the air):
 *
 *    join request  MHDR 00 | JoinEUI 8 | DevEUI 8 | DevNonce 2 | MIC 4      23 bytes
 *    join accept   MHDR 20 | JoinNonce 3 | NetID 3 | DevAddr 4 | DLSettings 1
 *                          | RxDelay 1 | [CFList 16] | MIC 4                17 or 33
 *                  (everything after MHDR is encrypted: the device runs AES
 *                   *encrypt* over it, as the server decrypted to make it)
 *    data          MHDR | DevAddr 4 | FCtrl 1 | FCnt 2 | FOpts 0-15
 *                       | [FPort 1 | FRMPayload] | MIC 4
 *
 *  Keys (1.0.x, the AppKey is the root): NwkSKey = AES(AppKey, 01 |
 *  JoinNonce | NetID | DevNonce | 0...), AppSKey the same with 02. The MIC
 *  is the first 4 bytes of AES-CMAC: over MHDR..DevNonce with the AppKey
 *  for a join request, over MHDR..CFList for a join accept, and over
 *  B0 | the frame for data (B0 = 49 00000000 dir DevAddr FCnt32 00 len).
 *  FRMPayload is XORed with AES(key, A_i), A_i = 01 00000000 dir DevAddr
 *  FCnt32 00 i, i from 1; the key is the NwkSKey on port 0, else the AppSKey.
 *  FOpts are not encrypted in 1.0.x.
 *
 *  From the LoRaWAN 1.0.x layouts as implemented in Semtech's LoRaMac-node
 *  (LoRaMacCrypto.c, LoRaMacSerializer.c, LoRaMacParser.c); checked against
 *  the independent lora-packet library (see test/lorawan_frame_test.cpp).
 */

#ifndef LORAWANFRAME_H_
#define LORAWANFRAME_H_

#include <stdint.h>

namespace lorawan {

enum MType : uint8_t {
	mtype_join_request     = 0,
	mtype_join_accept      = 1,
	mtype_unconfirmed_up   = 2,
	mtype_unconfirmed_down = 3,
	mtype_confirmed_up     = 4,
	mtype_confirmed_down   = 5,
	mtype_proprietary      = 7
};

inline uint8_t mhdr(MType t) { return (uint8_t)(t << 5); }   // Major 0: LoRaWAN R1
inline MType mtypeOf(uint8_t mhdrByte) { return (MType)(mhdrByte >> 5); }

// FCtrl bits.
static const uint8_t kFCtrlAdr       = 0x80;
static const uint8_t kFCtrlAdrAckReq = 0x40;   // uplink
static const uint8_t kFCtrlAck       = 0x20;
static const uint8_t kFCtrlFPending  = 0x10;   // downlink
static const uint8_t kFOptsLenMask   = 0x0F;

static const uint8_t kDirUp = 0, kDirDown = 1;

static const uint8_t kJoinRequestLen = 23;
static const uint8_t kMicLen = 4;
static const uint8_t kDataOverhead = 1 + 4 + 1 + 2 + kMicLen;   // MHDR, DevAddr, FCtrl, FCnt, MIC
static const uint8_t kMaxFOpts = 15;

// The join request for this DevNonce, with its MIC, into out.
void buildJoinRequest(const uint8_t appKey[16], const uint8_t joinEui[8], const uint8_t devEui[8],
                      uint16_t devNonce, uint8_t out[kJoinRequestLen]);

struct JoinAccept {
	uint32_t joinNonce;      // 24 bits
	uint32_t netId;          // 24 bits
	uint32_t devAddr;
	uint8_t  dlSettings;     // bit 7 OptNeg (1.1), 6..4 RX1DROffset, 3..0 RX2DataRate
	uint8_t  rxDelay;        // 3..0 Del (0 means 1 s)
	bool     hasCfList;
	uint8_t  cfList[16];
};

// Decrypts and checks a join accept (MHDR included, 17 or 33 bytes). The
// buffer is decrypted in place. False on a wrong MHDR or length, or a MIC
// that doesn't match.
bool parseJoinAccept(const uint8_t appKey[16], uint8_t* frame, uint8_t len, JoinAccept* ja);

void deriveSessionKeys(const uint8_t appKey[16], uint32_t joinNonce, uint32_t netId, uint16_t devNonce,
                       uint8_t nwkSKey[16], uint8_t appSKey[16]);

// XORs FRMPayload with the key stream (encrypts and decrypts alike).
void cryptPayload(const uint8_t key[16], uint8_t dir, uint32_t devAddr, uint32_t fCnt,
                  uint8_t* data, uint8_t len);

// The data frame MIC over msg (MHDR up to the end of FRMPayload).
void dataMic(const uint8_t nwkSKey[16], uint8_t dir, uint32_t devAddr, uint32_t fCnt,
             const uint8_t* msg, uint8_t len, uint8_t mic[kMicLen]);

struct Uplink {
	bool           confirmed;
	uint32_t       devAddr;
	uint8_t        fCtrl;        // ADR, ADRACKReq, ACK (FOptsLen is filled in)
	uint32_t       fCnt;         // 32 bits; the low 16 go on the air
	const uint8_t* fOpts;        // MAC commands, at most 15 bytes
	uint8_t        fOptsLen;
	bool           hasPort;      // false: no FPort and no payload
	uint8_t        port;         // 0: payload is MAC commands (then fOptsLen must be 0)
	const uint8_t* payload;      // plain text
	uint8_t        payloadLen;
};

// Builds, encrypts and signs an uplink into out. Returns the length, or
// 0 if it doesn't fit in cap or the fields are inconsistent.
uint8_t buildUplink(const uint8_t nwkSKey[16], const uint8_t appSKey[16], const Uplink& u,
                    uint8_t* out, uint8_t cap);

struct Downlink {
	MType    mtype;
	uint32_t devAddr;
	uint8_t  fCtrl;
	uint16_t fCnt16;          // as sent; the caller works out the 32-bit value
	uint8_t  fOptsOff, fOptsLen;
	bool     hasPort;
	uint8_t  port;
	uint8_t  payloadOff, payloadLen;
};

// Splits a data downlink (unconfirmed or confirmed) into its fields;
// checks nothing cryptographic. False if it isn't a well-formed downlink.
bool parseDownlinkHeader(const uint8_t* frame, uint8_t len, Downlink* d);

// Checks the MIC for this 32-bit FCnt, and if it matches decrypts the
// FRMPayload in place (with the NwkSKey on port 0, else the AppSKey).
bool openDownlink(const uint8_t nwkSKey[16], const uint8_t appSKey[16], const Downlink& d,
                  uint32_t fCnt, uint8_t* frame, uint8_t len);

} // namespace lorawan

#endif /* LORAWANFRAME_H_ */
