/*
 * MeshNode.h
 *
 *  A MeshCore node on any lora::iLoRaRadio (rfm95): it listens all the
 *  time (RX continuous), hears and verifies other nodes' adverts, reads
 *  group channels it has the key for (text and data), and sends its own
 *  adverts and group packets by flood, for MeshCore repeaters to carry.
 *  It doesn't repeat other nodes' packets, nor do direct messages, ACKs
 *  or paths (later work).
 *
 *      rfm95_param_t rp = rfm95_default_param();
 *      rp.syncWord = 0x12;            // MeshCore's (RadioLib's private) sync word
 *      rp.dio0Interrupt = true;
 *      rfm95<Stm32HalSPITransport> radio(rp, &hspi1, RFM_CS_GPIO_Port, RFM_CS_Pin, spi1Mutex);
 *      meshcore::LocalIdentity id;  id.fromSeed(seed);   // keep the seed
 *      meshcore::Node node(radio, id, meshcore::defaultNodeParam());
 *      int8_t pub = node.addChannel(meshcore::kPublicChannelKey, 16);
 *      node.begin(HAL_GetTick());
 *      for (;;) {
 *          node.main(HAL_GetTick());          // runs the radio too
 *          meshcore::NodeEvent e; uint8_t buf[184];
 *          while (node.takeEvent(&e, buf, sizeof buf)) ...
 *          node.sendGroupData(ch, 0xFF00, reading, n);   // when there is something to say
 *      }
 *
 *  The sync word is the radio's (rfm95_param_t::syncWord, set when the
 *  radio starts): 0x12 for MeshCore, 0x34 for LoRaWAN. A device runs one
 *  or the other.
 *
 *  Sending follows MeshCore's Dispatcher and Mesh (v1.12+):
 *  - an airtime budget: dutyPercent of each hour (MeshCore: 50%, its
 *    airtime factor 1.0), refilled as time passes; a packet waits until
 *    the budget holds at least half a 255 byte packet's airtime;
 *  - listen before talk: while a packet is arriving (channelBusy()) the
 *    send waits 120-480 ms at random and tries again, for up to 4 s, then
 *    sends anyway (MeshCore does the same);
 *  - its own packets' hashes go in the duplicate table, so a repeater's
 *    copy coming back is dropped.
 *  Received packets are dropped if malformed, of a payload version other
 *  than 1, or already seen (the last kSeen packet hashes).
 *
 *  Timestamps (adverts, group text) are the caller's: seconds since
 *  1970, from an RTC or SNTP. MeshCore nodes use them to order adverts
 *  and to tell repeated messages apart.
 *
 *  Not here: repeating, direct and anonymous messages (ECDH), ACKs, path
 *  learning, transport codes / regions (packets with them are read, ours
 *  are sent without), multipart, trace, CAD.
 */

#ifndef MESHNODE_H_
#define MESHNODE_H_

#include <stdint.h>
#include "iLoRaRadio.h"
#include "MeshPacket.h"
#include "MeshCrypto.h"
#include "MeshIdentity.h"
#include "MeshMessages.h"

namespace meshcore {

struct NodeParam {
	lora::Config radio;            // frequency, SF, bandwidth, CR, preamble, power
	uint8_t  pathHashSize;         // 1-3: the hash size repeaters add to our floods' paths
	uint8_t  dutyPercent;          // airtime budget, % of an hour
	uint16_t lbtMaxMs;             // listen-before-talk gives up waiting after this
	uint32_t randomSeed;
};

// US: 910.525 MHz, SF7, 62.5 kHz, CR 4/5, preamble 32, 17 dBm (the
// community's USA/Canada preset as remembered, not checked here: confirm
// with your local mesh). Preamble 32 at SF7-8 and 16 above, as MeshCore's
// radio wrappers set it.
NodeParam defaultNodeParam();
uint16_t preambleForSf(uint8_t sf);

enum NodeEventKind : uint8_t {
	node_ev_advert,          // a verified advert: pubKey, timestamp, advert, hops
	node_ev_group_text,      // channel, timestamp, the text (with "sender: ") in buf
	node_ev_group_data,      // channel, dataType, the data in buf
	node_ev_sent,            // one of ours went out
	node_ev_fault            // the radio failed; it restarts by itself
};

struct NodeEvent {
	NodeEventKind kind;
	uint8_t    channel;
	uint32_t   timestamp;
	uint16_t   dataType;
	uint8_t    len;            // bytes put in buf (text without its terminator)
	uint8_t    pubKey[kPubKeySize];
	AdvertData advert;
	bool       advertDataValid;
	uint8_t    hops;           // repeaters it came through (path hop count)
	PayloadType type;          // sent: what went out
	int16_t    rssiDbm;
	int8_t     snrDb;
	uint32_t   ticks;          // the radio's time stamp of the packet's end
};

struct NodeStats {
	uint32_t received, duplicates, malformed, ignored, badAdverts, unopened;
	uint32_t sent, lbtWaits, lbtForced, budgetWaits, eventsDropped, queueFull;
};

class Node {
public:
	static const uint8_t kChannels = 4;
	static const uint8_t kSeen = 64;       // packet hashes remembered (MeshCore: 160)
	static const uint8_t kQueue = 3;       // packets waiting to go out
	static const uint8_t kEvents = 3;
	static const uint32_t kBudgetWindowMs = 3600000u;

	Node(lora::iLoRaRadio& radio, const LocalIdentity& id, const NodeParam& param);

	void begin(uint32_t nowMs);
	void main(uint32_t nowMs);

	// A channel key (16 or 32 bytes). Returns its index, or -1 if full or
	// the key is the wrong length.
	int8_t addChannel(const uint8_t* key, uint8_t len);

	// Queue a packet. False if the queue is full or it doesn't fit.
	bool sendAdvert(uint32_t timestamp, const AdvertData& data, bool flood = true);
	bool sendGroupText(uint8_t channel, uint32_t timestamp, const char* sender, const char* text);
	bool sendGroupData(uint8_t channel, uint16_t dataType, const uint8_t* data, uint8_t len);
	// Any packet already built (its route and path as they are).
	bool sendPacket(const Packet& pkt);

	bool takeEvent(NodeEvent* e, uint8_t* buf, uint8_t cap);

	uint8_t queued() const { return _qCount; }
	const NodeStats& stats() const { return _st; }
	uint32_t budgetMs() const { return _budgetMs; }
	// How long main() may sleep (ms). Wire DIO0 to an interrupt that wakes
	// the thread (xMeshNode.h) so a received packet is read at once.
	uint32_t sleepHintMs(uint32_t nowMs) const;

	// The duplicate table, for tests.
	bool seen(const uint8_t hash[kHashSize]) const;

private:
	struct Slot { uint8_t len; PayloadType type; uint8_t raw[kMaxPacket]; };
	struct Ev { NodeEvent e; uint8_t data[kMaxPayload]; };

	uint32_t rnd();
	void markSeen(const uint8_t hash[kHashSize]);
	void handleRx(const uint8_t* raw, uint8_t len, int16_t rssi, int8_t snr, uint32_t ticks);
	void push(const NodeEvent& e, const uint8_t* data, uint8_t len);
	void refill(uint32_t nowMs);
	void trySend(uint32_t nowMs);
	void listen();

	lora::iLoRaRadio& _radio;
	const LocalIdentity& _id;
	NodeParam _p;
	uint32_t _rng;

	Channel _ch[kChannels];
	uint8_t _chCount = 0;

	uint8_t _seen[kSeen][kHashSize];
	uint8_t _seenNext = 0, _seenCount = 0;

	Slot    _q[kQueue];
	uint8_t _qHead = 0, _qCount = 0;
	bool    _sending = false;
	bool    _listening = false;        // we put the radio in RX continuous and nothing ended it
	PayloadType _sendingType = payload_raw_custom;
	uint32_t _sendStartMs = 0, _sendAirMs = 0;
	uint32_t _nextTryMs = 0;
	bool     _lbtWaiting = false;
	uint32_t _lbtStartMs = 0;

	uint32_t _budgetMs = 0, _budgetAtMs = 0;
	bool     _started = false;

	Ev      _ev[kEvents];
	uint8_t _evHead = 0, _evCount = 0;

	uint8_t _rx[kMaxPacket];
	NodeStats _st = {};
};

} // namespace meshcore

#endif /* MESHNODE_H_ */
