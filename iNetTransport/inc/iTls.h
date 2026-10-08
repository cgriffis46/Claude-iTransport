#pragma once
#include <cstddef>
#include <cstdint>

// The seam between a server and a TLS library, so xHttpServer (and the
// FreeRTOS layer) never sees mbedTLS's headers. tls/MbedTlsServer is the
// implementation.

// The connection under TLS: bytes in and out, sleeping up to a timeout.
class TlsIo {
public:
    virtual ~TlsIo() = default;
    // > 0: bytes received; 0: the timeout ran out; -1: closed.
    virtual int32_t recv(uint8_t* buf, size_t len, uint32_t timeoutMs) = 0;
    // > 0: bytes queued; <= 0: closed or stalled.
    virtual int32_t send(const uint8_t* buf, size_t len, uint32_t timeoutMs) = 0;
};

// One TLS connection, used by one thread.
class iTlsSession {
public:
    virtual ~iTlsSession() = default;
    // The handshake, over io. false: it failed (the peer isn't speaking
    // TLS, wants something we don't offer, or went quiet): close.
    virtual bool    handshake(TlsIo& io, uint32_t timeoutMs) = 0;
    // As xClient::read(): > 0 bytes, 0 nothing within the timeout, -1 closed.
    virtual int32_t read(uint8_t* buf, size_t len, uint32_t timeoutMs) = 0;
    // All of it: len, or -1.
    virtual int32_t write(const uint8_t* buf, size_t len, uint32_t timeoutMs) = 0;
    // Says goodbye (close_notify) if the handshake was done, and frees the
    // session's buffers. The connection itself is the caller's to close.
    virtual void    close() = 0;
};

// Hands out sessions, one per connection.
class iTlsServer {
public:
    virtual ~iTlsServer() = default;
    virtual iTlsSession* acquire() = 0;           // nullptr: none free
    virtual void         release(iTlsSession* s) = 0;
};
