#include "Stm32CanSafetyBroadcaster.h"
#include "Crc8.h"

Stm32CanSafetyBroadcaster::Stm32CanSafetyBroadcaster(Safe& source, iTransport& transport1,
                                                       iTransport& transport2, uint32_t serialCode)
    : source_(source), transport1_(transport1), transport2_(transport2),
      lowerHalf_(static_cast<uint16_t>(serialCode & 0xFFFFu)),
      upperHalf_(static_cast<uint16_t>((serialCode >> 16) & 0xFFFFu)) {}

void Stm32CanSafetyBroadcaster::buildFrame(uint8_t* outFrame, uint16_t codeHalf, bool safe) const {
    outFrame[0] = static_cast<uint8_t>(codeHalf & 0xFF);
    outFrame[1] = static_cast<uint8_t>((codeHalf >> 8) & 0xFF);
    outFrame[2] = safe ? 0x01 : 0x00;
    outFrame[3] = static_cast<uint8_t>(outFrame[2] ^ 0xFF); // bitwise-inverted copy of byte 2
    outFrame[4] = sequenceCounter_;                          // shared — same value for BOTH frames this round
    outFrame[5] = Crc8::compute(outFrame, 5);
}

void Stm32CanSafetyBroadcaster::broadcast() {
    uint8_t frame1[6];
    uint8_t frame2[6];

    // Both frames built using the CURRENT sequenceCounter_ value —
    // increment happens once, afterward, for the NEXT round.
    buildFrame(frame1, lowerHalf_, source_.GetSafe1State());
    buildFrame(frame2, upperHalf_, source_.GetSafe2State());

    transport1_.write(frame1, sizeof(frame1));
    transport2_.write(frame2, sizeof(frame2));

    ++sequenceCounter_;
}
