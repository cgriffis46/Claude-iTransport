#include "CipTagMessageCodec.h"

void CipTagMessageCodec::writeErrorResponse(uint8_t requestService, uint8_t generalStatus,
                                             std::vector<uint8_t>& outResponse) {
    outResponse.clear();
    outResponse.push_back(static_cast<uint8_t>(requestService | 0x80u)); // Reply Service
    outResponse.push_back(0x00u);                                       // Reserved, shall be zero
    outResponse.push_back(generalStatus);                               // General Status
    outResponse.push_back(0x00u);                                       // Size of Additional Status (words)
    // No Additional Status, no Response Data for an error.
}

bool CipTagMessageCodec::parseSymbolicTagName(const std::vector<uint8_t>& request, size_t offset,
                                               size_t pathLengthBytes, std::string& outTagName) {
    // Need at least the 0x91 segment-type byte and a length byte.
    if (pathLengthBytes < 2 || offset + pathLengthBytes > request.size()) {
        return false;
    }
    if (request[offset] != 0x91u) {
        return false; // only the ANSI Extended Symbol segment is supported
    }
    const uint8_t nameLen = request[offset + 1];
    const size_t expectedLen = 2 + nameLen + (nameLen % 2); // +1 pad byte if nameLen is odd
    if (expectedLen != pathLengthBytes) {
        return false; // path contains more than just this one symbolic segment, or is truncated
    }
    if (offset + 2 + nameLen > request.size()) {
        return false; // name would read past the buffer
    }

    outTagName.assign(reinterpret_cast<const char*>(&request[offset + 2]), nameLen);
    return true;
}

void CipTagMessageCodec::handleRequest(PlcTagRegistry& registry, const std::vector<uint8_t>& request,
                                        std::vector<uint8_t>& outResponse) {
    // Minimum possible request: Service (1) + Request_Path_Size (1) = 2 bytes,
    // before even checking the path itself exists.
    if (request.size() < 2) {
        // Not even enough data to know which service was requested --
        // service code 0x00 is not a real CIP service, used here only
        // so writeErrorResponse has SOMETHING to OR 0x80 into; a
        // client sending a request this malformed has bigger problems
        // than an accurate reply-service byte.
        writeErrorResponse(0x00, CipGeneralStatus::NotEnoughData, outResponse);
        return;
    }

    const uint8_t service = request[0];
    const uint8_t pathSizeWords = request[1];
    const size_t pathSizeBytes = static_cast<size_t>(pathSizeWords) * 2;
    const size_t pathOffset = 2;

    if (service != CipServiceCode::GetAttributeSingle && service != CipServiceCode::SetAttributeSingle) {
        writeErrorResponse(service, CipGeneralStatus::ServiceNotSupported, outResponse);
        return;
    }

    if (pathOffset + pathSizeBytes > request.size()) {
        writeErrorResponse(service, CipGeneralStatus::NotEnoughData, outResponse);
        return;
    }

    std::string tagName;
    if (!parseSymbolicTagName(request, pathOffset, pathSizeBytes, tagName)) {
        writeErrorResponse(service, CipGeneralStatus::PathSegmentError, outResponse);
        return;
    }

    const size_t dataOffset = pathOffset + pathSizeBytes;

    if (service == CipServiceCode::GetAttributeSingle) {
        size_t tagSize; PlcDataType tagType; bool writable;
        if (!registry.describeTag(tagName, tagSize, tagType, writable)) {
            writeErrorResponse(service, CipGeneralStatus::PathDestinationUnknown, outResponse);
            return;
        }

        std::vector<uint8_t> value(tagSize);
        if (!registry.readTag(tagName, value.data(), value.size())) {
            // Can only happen if the tag was removed between describeTag()
            // and readTag() -- this registry has no remove operation today,
            // so unreachable in practice, but handled rather than assumed.
            writeErrorResponse(service, CipGeneralStatus::PathDestinationUnknown, outResponse);
            return;
        }

        outResponse.clear();
        outResponse.push_back(static_cast<uint8_t>(service | 0x80u));
        outResponse.push_back(0x00u);
        outResponse.push_back(CipGeneralStatus::Success);
        outResponse.push_back(0x00u);
        outResponse.insert(outResponse.end(), value.begin(), value.end());
        return;
    }

    // SetAttributeSingle
    size_t tagSize; PlcDataType tagType; bool writable;
    if (!registry.describeTag(tagName, tagSize, tagType, writable)) {
        writeErrorResponse(service, CipGeneralStatus::PathDestinationUnknown, outResponse);
        return;
    }
    if (!writable) {
        writeErrorResponse(service, CipGeneralStatus::AttributeNotSettable, outResponse);
        return;
    }

    const size_t writeDataLen = request.size() - dataOffset;
    if (writeDataLen < tagSize) {
        writeErrorResponse(service, CipGeneralStatus::NotEnoughData, outResponse);
        return;
    }
    if (writeDataLen > tagSize) {
        writeErrorResponse(service, CipGeneralStatus::TooMuchData, outResponse);
        return;
    }

    if (!registry.writeTag(tagName, &request[dataOffset], writeDataLen)) {
        // Shouldn't happen given the checks above, but never assumed.
        writeErrorResponse(service, CipGeneralStatus::PathDestinationUnknown, outResponse);
        return;
    }

    outResponse.clear();
    outResponse.push_back(static_cast<uint8_t>(service | 0x80u));
    outResponse.push_back(0x00u);
    outResponse.push_back(CipGeneralStatus::Success);
    outResponse.push_back(0x00u);
    // No Response Data for a successful Set_Attribute_Single.
}
