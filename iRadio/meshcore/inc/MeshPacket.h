/*
 * MeshPacket.h
 *
 *  A MeshCore packet (protocol version 1, as MeshCore firmware v1.12+
 *  sends it). On the air:
 *
 *    header 1 | [transport codes 2 x 2] | path_len 1 | path | payload
 *
 *    header    bits 7..6 payload version, 5..2 payload type, 1..0 route type
 *    route     0 transport flood, 1 flood, 2 direct, 3 transport direct
 *              (transport codes only with 0 and 3, little-endian)
 *    path_len  bits 7..6 hash size - 1 (1-3 bytes), 5..0 hop count (0-63);
 *              the path is hop count x hash size bytes, at most 64
 *    payload   1..184 bytes, the rest of the packet
 *
 *  The packet hash, which MeshCore uses to drop duplicates, is the first
 *  8 bytes of SHA-256 over the payload type byte and the payload (TRACE
 *  packets also hash path_len, as a 16-bit value: MeshCore hashes its
 *  uint16_t field).
 *
 *  From MeshCore's docs/packet_format.md and src/Packet.cpp; checked
 *  against packets parsed and hashed by MeshCore's own Packet.cpp
 *  (iRadio/test/meshcore_packet_test.cpp). Pure logic, no heap.
 */

#ifndef MESHPACKET_H_
#define MESHPACKET_H_

#include <stdint.h>

namespace meshcore {

static const uint8_t kMaxPayload = 184;   // MAX_PACKET_PAYLOAD
static const uint8_t kMaxPath = 64;       // MAX_PATH_SIZE
static const uint8_t kMaxPacket = 255;    // MAX_TRANS_UNIT
static const uint8_t kHashSize = 8;       // MAX_HASH_SIZE: the packet hash kept for duplicates

enum RouteType : uint8_t {
	route_transport_flood = 0,
	route_flood = 1,
	route_direct = 2,
	route_transport_direct = 3
};

enum PayloadType : uint8_t {
	payload_req = 0x00,
	payload_response = 0x01,
	payload_txt_msg = 0x02,
	payload_ack = 0x03,
	payload_advert = 0x04,
	payload_grp_txt = 0x05,
	payload_grp_data = 0x06,
	payload_anon_req = 0x07,
	payload_path = 0x08,
	payload_trace = 0x09,
	payload_multipart = 0x0A,
	payload_control = 0x0B,
	payload_raw_custom = 0x0F
};

struct Packet {
	uint8_t  header = 0;
	uint16_t transport[2] = {0, 0};
	uint8_t  pathLen = 0;            // as on the air: hash size and hop count
	uint8_t  path[kMaxPath];
	uint8_t  payload[kMaxPayload];
	uint8_t  payloadLen = 0;

	RouteType routeType() const { return (RouteType)(header & 0x03); }
	PayloadType payloadType() const { return (PayloadType)((header >> 2) & 0x0F); }
	uint8_t payloadVersion() const { return (uint8_t)(header >> 6); }
	bool hasTransportCodes() const { return routeType() == route_transport_flood || routeType() == route_transport_direct; }
	bool isFlood() const { return routeType() == route_transport_flood || routeType() == route_flood; }

	uint8_t pathHashSize() const { return (uint8_t)((pathLen >> 6) + 1); }
	uint8_t hopCount() const { return (uint8_t)(pathLen & 63); }
	uint8_t pathBytes() const { return (uint8_t)(hopCount() * pathHashSize()); }

	// A new packet: version 1, this type and route, no path yet.
	void begin(PayloadType t, RouteType r, uint8_t hashSize = 1) {
		header = (uint8_t)((t << 2) | r);
		transport[0] = transport[1] = 0;
		pathLen = (uint8_t)((hashSize - 1) << 6);
		payloadLen = 0;
	}

	// The bytes on the air. Returns the length, 0 if the packet is invalid.
	uint8_t encode(uint8_t* out, uint8_t cap) const;
	// False on a malformed packet (bad path length, no payload, too long).
	bool decode(const uint8_t* in, uint8_t len);

	void hash(uint8_t out[kHashSize]) const;

	static bool validPathLen(uint8_t pathLen);
};

} // namespace meshcore

#endif /* MESHPACKET_H_ */
