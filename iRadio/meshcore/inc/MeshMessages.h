/*
 * MeshMessages.h
 *
 *  The MeshCore payloads this node speaks (payload version 1), built into
 *  and read from a Packet. Pure logic, no radio, no heap.
 *
 *  Advert (payload type 4), sent flood, or zero hop (direct, no path):
 *    public key 32 | timestamp 4 | signature 64 | app data 0-32
 *    The signature (Ed25519) covers public key | timestamp | app data.
 *    App data: flags 1 (bits 3..0 type: 1 chat, 2 repeater, 3 room,
 *    4 sensor; 0x10 location, 0x20 feature 1, 0x40 feature 2, 0x80 name)
 *    | [lat 4 | lon 4, degrees x 1e6] | [feature 1 2] | [feature 2 2]
 *    | [name: the rest, UTF-8, cut at a whole character].
 *    A receiver signs over at most 32 bytes of app data, as MeshCore does.
 *
 *  Group text (5) and group data (6), sent flood:
 *    channel hash 1 | MAC 2 | ciphertext   (see MeshCrypto.h)
 *    text plaintext: timestamp 4 | flags 1 (0: plain) | "sender: message"
 *      (the message cut to 160 bytes with its prefix, MAX_TEXT_LEN)
 *    data plaintext: data type 2 | length 1 | data (at most 165 bytes);
 *      data types FF00-FFFF are free for development, others are
 *      allocated in MeshCore's docs/number_allocations.md.
 *    Group packets carry no sender signature: anyone with the channel key
 *    can write any sender name.
 *
 *  All multi-byte fields little-endian. From MeshCore's docs/payloads.md,
 *  src/Mesh.cpp (createAdvert, createGroupDatagram, onRecvPacket) and
 *  src/helpers/BaseChatMesh.cpp and AdvertDataHelpers.cpp; checked against
 *  packets made by MeshCore's own code (meshcore_packet_test).
 */

#ifndef MESHMESSAGES_H_
#define MESHMESSAGES_H_

#include <stdint.h>
#include "MeshPacket.h"
#include "MeshCrypto.h"
#include "MeshIdentity.h"

namespace meshcore {

static const uint8_t kMaxAdvertData = 32;           // MAX_ADVERT_DATA_SIZE
static const uint8_t kMaxTextLen = 160;             // MAX_TEXT_LEN: prefix and message
static const uint8_t kMaxGroupData = kMaxPayload - 16 - 3;   // MAX_GROUP_DATA_LENGTH, 165

enum AdvertType : uint8_t { adv_none = 0, adv_chat = 1, adv_repeater = 2, adv_room = 3, adv_sensor = 4 };

struct AdvertData {
	uint8_t  type = adv_sensor;
	bool     hasLocation = false;
	int32_t  latE6 = 0, lonE6 = 0;   // degrees x 1e6
	uint16_t feature1 = 0, feature2 = 0;   // sent when not 0
	char     name[kMaxAdvertData + 1] = {0};

	// App data bytes into out (kMaxAdvertData); returns the length.
	uint8_t encode(uint8_t* out) const;
	// False if the fields run past len.
	bool decode(const uint8_t* in, uint8_t len);
};

struct Advert {
	uint8_t    pubKey[kPubKeySize];
	uint32_t   timestamp;
	uint8_t    appData[kMaxAdvertData];
	uint8_t    appLen;
	AdvertData data;
	bool       dataValid;
};

// A signed advert into pkt (flood, or zero hop: direct with no path).
bool buildAdvert(const LocalIdentity& id, uint32_t timestamp, const AdvertData& data, bool flood, Packet& pkt);
// The same from app data bytes already encoded (at most kMaxAdvertData).
bool buildAdvertRaw(const LocalIdentity& id, uint32_t timestamp, const uint8_t* app, uint8_t appLen, bool flood, Packet& pkt);
// Reads and verifies an advert. False if short or the signature fails.
bool parseAdvert(const Packet& pkt, Advert* a);

// Group text or data, flood, into pkt. False if too long.
bool buildGroupText(const Channel& ch, uint32_t timestamp, const char* sender, const char* text, Packet& pkt);
bool buildGroupData(const Channel& ch, uint16_t dataType, const uint8_t* data, uint8_t len, Packet& pkt);
// Either from a plaintext already laid out (MeshCore's createGroupDatagram).
bool buildGroupPacket(const Channel& ch, PayloadType t, const uint8_t* plain, uint8_t len, Packet& pkt);

struct GroupMessage {
	uint8_t  channel;                // index into the channels given
	bool     isText;
	uint32_t timestamp;              // text
	uint8_t  textFlags;              // text: 0 plain (upper 6 bits the type)
	uint16_t dataType;               // data
	uint8_t  data[kMaxPayload];      // text (zero terminated) or data
	uint8_t  len;                    // bytes in data (the text without its terminator)
};

// Tries each channel whose hash matches; false if none opens it (no
// match, a bad MAC, or a malformed plaintext).
bool openGroup(const Packet& pkt, const Channel* channels, uint8_t count, GroupMessage* m);

// The longest prefix of text, at most max bytes, that ends on a whole
// UTF-8 character (MeshCore's validUtf8PrefixLength).
uint8_t utf8Prefix(const char* text, uint8_t max);

} // namespace meshcore

#endif /* MESHMESSAGES_H_ */
