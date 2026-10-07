#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>
#include <string>
#include "PlcTagRegistry.h"

// General status codes used by this codec — ODVA Volume 1, Appendix
// B, verified directly against the actual table.
namespace CipGeneralStatus {
    constexpr uint8_t Success                = 0x00;
    constexpr uint8_t PathSegmentError        = 0x04; // malformed/unsupported path segment
    constexpr uint8_t PathDestinationUnknown  = 0x05; // tag name not found in the registry
    constexpr uint8_t ServiceNotSupported     = 0x08; // anything other than Get/Set_Attribute_Single
    constexpr uint8_t AttributeNotSettable    = 0x0E; // write attempted on a read-only tag
    constexpr uint8_t NotEnoughData           = 0x13; // request too short, or write data size mismatch
    constexpr uint8_t TooMuchData             = 0x15; // write supplied more data than the tag's size
}

namespace CipServiceCode {
    constexpr uint8_t GetAttributeSingle = 0x0E;
    constexpr uint8_t SetAttributeSingle = 0x10;
}

// Encodes and decodes CIP explicit messages (ODVA Volume 1, Table
// 2-4.1/2-4.2: Message Router Request/Response Format) for reading
// and writing PLC tags BY NAME, using the standard ANSI Extended
// Symbol segment (0x91) for addressing — the same mechanism real CIP
// controllers use for tag-name-based addressing, verified directly
// against Table 3-5.15's own worked example ("tag1" encoded as
// 91 04 74 61 67 31).
//
// Deliberately narrow scope, stated plainly rather than implied: this
// handles ONLY Get_Attribute_Single/Set_Attribute_Single requests
// whose path is a single symbolic segment naming a tag in the given
// PlcTagRegistry. It does not implement general CIP message routing,
// multiple application paths, Unconnected_Send encapsulation, or the
// rest of EPATH's segment types (port/link, logical class/instance/
// attribute, data segments). This is enough real CIP wire format to
// read and write named tags — verified against the actual spec
// tables — not a general-purpose CIP message router.
//
// Every public entry point treats its input as untrusted wire data:
// bounds-checked throughout, never trusts a length field without
// verifying the buffer actually holds that many bytes, and always
// produces a well-formed CIP error response rather than crashing or
// silently doing nothing on malformed input.
class CipTagMessageCodec {
public:
    // Parses a raw Message Router Request and, if well-formed,
    // performs the corresponding PlcTagRegistry operation, writing a
    // complete Message Router Response (reply service, reserved byte,
    // general status, and response data) into outResponse. Always
    // produces a valid response, even for malformed input or an
    // unknown tag — a General Status error response, never silence,
    // since leaving a client with no response at all would hang its
    // connection rather than tell it anything.
    static void handleRequest(PlcTagRegistry& registry, const std::vector<uint8_t>& request,
                               std::vector<uint8_t>& outResponse);

private:
    // Parses a Padded EPATH expected to contain exactly one ANSI
    // Extended Symbol segment (0x91), reading pathLengthBytes bytes
    // starting at request[offset]. Returns false if the path isn't a
    // single, well-formed symbolic segment, or would read past the
    // buffer.
    static bool parseSymbolicTagName(const std::vector<uint8_t>& request, size_t offset,
                                      size_t pathLengthBytes, std::string& outTagName);

    static void writeErrorResponse(uint8_t requestService, uint8_t generalStatus,
                                    std::vector<uint8_t>& outResponse);
};
