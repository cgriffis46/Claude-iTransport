#pragma once
#include <cstddef>
#include <cstdint>
#include "iLock.h"
#include "iTls.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/pk.h"
#include "mbedtls/ssl.h"
#include "mbedtls/ssl_ticket.h"
#include "mbedtls/x509_crt.h"

// A TLS 1.2 server on mbedTLS, for xHttpServer (Config::tls):
//
//     static iLock* lock = ...;                         // a FreeRTOS mutex: TlsFreeRtos.h
//     MbedTlsServer::Config t;
//     t.certPem = deviceCertPem;   t.keyPem = deviceKeyPem;   // tools/make_web_cert.sh
//     t.lock = lock;
//     static MbedTlsServer tls(t);
//     tls.begin();
//     webCfg.tls = &tls;  webCfg.port = 443;
//
// Built with tls/config/inet_mbedtls_config.h: ECDHE-ECDSA with AES-GCM
// or ChaCha20-Poly1305, an ECDSA P-256 certificate, session tickets (a
// returning browser skips the ECC). Each session's buffers (about 25 KB:
// 16 KB in, 4 KB out, and state) come from mbedTLS's calloc only while
// its connection is open; point that at the FreeRTOS heap
// (TlsFreeRtos.h). Sessions are used by one thread each; what they
// share (the random generator, the ticket keys) is behind Config::lock.
class MbedTlsServer : public iTlsServer {
public:
    static constexpr uint8_t kMaxSessions = 4;

    typedef iLock Lock;

    struct Config {
        const char*   certPem = nullptr;    // the device's certificate, then any intermediates (PEM)
        const char*   keyPem = nullptr;     // its private key (PEM, unencrypted)
        uint8_t       sessions = 2;         // at most this many TLS connections at once (<= kMaxSessions)
        uint32_t      ticketLifetimeS = 86400;
        Lock*         lock = nullptr;       // needed when sessions run on more than one thread
        const char*   personalization = "iNetTransport TLS";
    };

    MbedTlsServer() : MbedTlsServer(Config()) {}
    explicit MbedTlsServer(const Config& cfg);
    ~MbedTlsServer() override;

    MbedTlsServer(const MbedTlsServer&) = delete;
    MbedTlsServer& operator=(const MbedTlsServer&) = delete;

    // Seeds the random generator and loads the certificate and key.
    // false: see error() (mbedTLS's code).
    bool begin();
    int  error() const { return error_; }

    iTlsSession* acquire() override;
    void         release(iTlsSession* s) override;

    // Random bytes from the server's generator, for session tokens and
    // the like (WebAuth::Config::random).
    bool random(uint8_t* out, size_t len);
    static bool randomFn(uint8_t* out, size_t len, void* self) {
        return static_cast<MbedTlsServer*>(self)->random(out, len);
    }

    uint32_t handshakes() const { return handshakes_; }
    uint32_t handshakeFailures() const { return failures_; }

private:
    class Session : public iTlsSession {
    public:
        bool    handshake(TlsIo& io, uint32_t timeoutMs) override;
        int32_t read(uint8_t* buf, size_t len, uint32_t timeoutMs) override;
        int32_t write(const uint8_t* buf, size_t len, uint32_t timeoutMs) override;
        void    close() override;

        MbedTlsServer*      server = nullptr;
        mbedtls_ssl_context ssl;
        TlsIo*              io = nullptr;
        uint32_t            timeoutMs = 0;
        bool                inUse = false, setUp = false, open = false;

        static int sendCb(void* self, const unsigned char* buf, size_t len);
        static int recvCb(void* self, unsigned char* buf, size_t len, uint32_t timeout);
    };

    // The shared pieces, taken under the lock.
    static int rngLocked(void* self, unsigned char* out, size_t len);
    static int ticketWrite(void* self, const mbedtls_ssl_session* s, unsigned char* start, const unsigned char* end,
                           size_t* len, uint32_t* lifetime);
    static int ticketParse(void* self, mbedtls_ssl_session* s, unsigned char* buf, size_t len);
    void lock() { if (cfg_.lock) cfg_.lock->lock(); }
    void unlock() { if (cfg_.lock) cfg_.lock->unlock(); }

    Config                   cfg_;
    bool                     ready_ = false;
    int                      error_ = 0;
    mbedtls_entropy_context  entropy_;
    mbedtls_ctr_drbg_context drbg_;
    mbedtls_x509_crt         cert_;
    mbedtls_pk_context       key_;
    mbedtls_ssl_config       conf_;
    mbedtls_ssl_ticket_context ticket_;
    Session                  sessions_[kMaxSessions];
    uint32_t                 handshakes_ = 0, failures_ = 0;
};
