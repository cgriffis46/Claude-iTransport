#pragma once
#include <cstdint>
#include <cstddef>
#include <netinet/in.h>
#include <pthread.h>
#include <atomic>
#include "iTransportEthernet.h"

// Real, POSIX-socket-based (UDP) implementation of iTransportEthernet.
// Genuinely testable, real networking — unlike anything CIP/EtherNet-
// IP-protocol-specific, this is standard BSD sockets, nothing
// proprietary or protocol-specific about it at this layer at all.
// write()/setRxSink() move raw UDP datagram payloads; what's actually
// INSIDE those payloads (a CIP Safety frame, an openSAFETY frame, or
// anything else) is entirely the concern of whatever
// SafetyProtocolCodec sits on top of this, not this class.
//
// UDP, not TCP: CIP's own "implicit" (I/O, real-time) messaging uses
// UDP conventionally — TCP's stream/connection semantics don't fit
// periodic safety broadcasts well, same reasoning CIP itself uses.
//
// Receiving is push-based via iTransportRxSink, same convention as
// every other transport in this codebase — but worth restating the
// same caveat noted for Stm32HalCanTransport: a UDP datagram, like a
// CAN frame, arrives as one atomic unit, not a continuous byte
// stream. Pushing its payload through onByteReceived() one byte at a
// time works, but it discards the fact the bytes arrived together as
// a single packet — fine for a fixed-length, single-purpose safety
// message where the consumer already knows to expect exactly N
// bytes, a poor fit if this ever needs to carry packets of varying
// purpose or length.
//
// A dedicated pthread handles receiving, same reasoning as pBMP280's
// own dedicated thread: recvfrom() blocks, so something has to be
// continuously waiting on it independent of whatever else is running.
class LinuxUdpTransport : public iTransportEthernet {
public:
    // remoteHost/remotePort: where write() sends datagrams.
    // localPort: the port this instance binds to and listens on for
    // incoming datagrams. Constructing this opens a real UDP socket
    // and starts the receiver thread immediately.
    LinuxUdpTransport(const char* remoteHost, uint16_t remotePort, uint16_t localPort);
    ~LinuxUdpTransport() override;

    bool write(const uint8_t* data, size_t len) override;
    void setRxSink(iTransportRxSink& sink) override;

private:
    void receiveLoop();
    static void* threadTrampoline(void* arg);

    int                sockFd_;
    struct sockaddr_in remoteAddr_{};
    iTransportRxSink*  rxSink_ = nullptr;
    pthread_t          recvThread_{};
    std::atomic<bool>  running_{true};
};
