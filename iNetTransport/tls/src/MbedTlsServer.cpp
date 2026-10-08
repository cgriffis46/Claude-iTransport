#include "MbedTlsServer.h"
#include <cstring>

namespace {
// mbedTLS's own code for a failed send (net_sockets.h, which this build
// doesn't otherwise need).
constexpr int kSendFailed = -0x004E;
}  // namespace

MbedTlsServer::MbedTlsServer(const Config& cfg) : cfg_(cfg) {
    if (cfg_.sessions == 0) cfg_.sessions = 1;
    if (cfg_.sessions > kMaxSessions) cfg_.sessions = kMaxSessions;
    mbedtls_entropy_init(&entropy_);
    mbedtls_ctr_drbg_init(&drbg_);
    mbedtls_x509_crt_init(&cert_);
    mbedtls_pk_init(&key_);
    mbedtls_ssl_config_init(&conf_);
    mbedtls_ssl_ticket_init(&ticket_);
    for (Session& s : sessions_) {
        s.server = this;
        mbedtls_ssl_init(&s.ssl);
    }
}

MbedTlsServer::~MbedTlsServer() {
    for (Session& s : sessions_) mbedtls_ssl_free(&s.ssl);
    mbedtls_ssl_ticket_free(&ticket_);
    mbedtls_ssl_config_free(&conf_);
    mbedtls_pk_free(&key_);
    mbedtls_x509_crt_free(&cert_);
    mbedtls_ctr_drbg_free(&drbg_);
    mbedtls_entropy_free(&entropy_);
}

bool MbedTlsServer::begin() {
    if (ready_) return true;
    if (cfg_.certPem == nullptr || cfg_.keyPem == nullptr) {
        error_ = MBEDTLS_ERR_SSL_BAD_INPUT_DATA;
        return false;
    }
    const char* pers = cfg_.personalization ? cfg_.personalization : "";
    // PEM parsing wants the terminating NUL counted.
    if ((error_ = mbedtls_ctr_drbg_seed(&drbg_, mbedtls_entropy_func, &entropy_,
                                        reinterpret_cast<const unsigned char*>(pers), std::strlen(pers))) != 0 ||
        (error_ = mbedtls_x509_crt_parse(&cert_, reinterpret_cast<const unsigned char*>(cfg_.certPem),
                                         std::strlen(cfg_.certPem) + 1)) != 0 ||
        (error_ = mbedtls_pk_parse_key(&key_, reinterpret_cast<const unsigned char*>(cfg_.keyPem),
                                       std::strlen(cfg_.keyPem) + 1, nullptr, 0, rngLocked, this)) != 0 ||
        (error_ = mbedtls_ssl_config_defaults(&conf_, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM,
                                              MBEDTLS_SSL_PRESET_DEFAULT)) != 0) {
        return false;
    }
    mbedtls_ssl_conf_rng(&conf_, rngLocked, this);
    mbedtls_ssl_conf_min_tls_version(&conf_, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_max_tls_version(&conf_, MBEDTLS_SSL_VERSION_TLS1_2);
    if ((error_ = mbedtls_ssl_conf_own_cert(&conf_, &cert_, &key_)) != 0) return false;
    // The ticket code draws random numbers itself, and only ever runs
    // inside ticketWrite()/ticketParse(), with the lock already held: it
    // gets the generator directly. (rngLocked would take the lock again.)
    if ((error_ = mbedtls_ssl_ticket_setup(&ticket_, mbedtls_ctr_drbg_random, &drbg_, MBEDTLS_CIPHER_AES_256_GCM,
                                           cfg_.ticketLifetimeS)) != 0) {
        return false;
    }
    mbedtls_ssl_conf_session_tickets_cb(&conf_, ticketWrite, ticketParse, this);
    ready_ = true;
    return true;
}

int MbedTlsServer::rngLocked(void* self, unsigned char* out, size_t len) {
    MbedTlsServer* me = static_cast<MbedTlsServer*>(self);
    me->lock();
    const int r = mbedtls_ctr_drbg_random(&me->drbg_, out, len);
    me->unlock();
    return r;
}

bool MbedTlsServer::random(uint8_t* out, size_t len) {
    // The generator gives at most MBEDTLS_CTR_DRBG_MAX_REQUEST bytes a call.
    while (len) {
        const size_t n = len < MBEDTLS_CTR_DRBG_MAX_REQUEST ? len : MBEDTLS_CTR_DRBG_MAX_REQUEST;
        if (!ready_ || rngLocked(this, out, n) != 0) return false;
        out += n;
        len -= n;
    }
    return true;
}

int MbedTlsServer::ticketWrite(void* self, const mbedtls_ssl_session* s, unsigned char* start,
                               const unsigned char* end, size_t* len, uint32_t* lifetime) {
    MbedTlsServer* me = static_cast<MbedTlsServer*>(self);
    me->lock();
    const int r = mbedtls_ssl_ticket_write(&me->ticket_, s, start, end, len, lifetime);
    me->unlock();
    return r;
}

int MbedTlsServer::ticketParse(void* self, mbedtls_ssl_session* s, unsigned char* buf, size_t len) {
    MbedTlsServer* me = static_cast<MbedTlsServer*>(self);
    me->lock();
    const int r = mbedtls_ssl_ticket_parse(&me->ticket_, s, buf, len);
    me->unlock();
    return r;
}

iTlsSession* MbedTlsServer::acquire() {
    if (!ready_) return nullptr;
    lock();
    Session* got = nullptr;
    for (uint8_t i = 0; i < cfg_.sessions && got == nullptr; ++i) {
        if (!sessions_[i].inUse) {
            got = &sessions_[i];
            got->inUse = true;
        }
    }
    unlock();
    return got;
}

void MbedTlsServer::release(iTlsSession* s) {
    if (s == nullptr) return;
    Session* k = static_cast<Session*>(s);
    k->close();
    lock();
    k->inUse = false;
    unlock();
}

// ---- a session ----

int MbedTlsServer::Session::sendCb(void* self, const unsigned char* buf, size_t len) {
    Session* s = static_cast<Session*>(self);
    const int32_t n = s->io->send(buf, len, s->timeoutMs);
    return n > 0 ? static_cast<int>(n) : kSendFailed;
}

// The timeout mbedTLS passes is its configuration's; each call's own
// (timeoutMs) is the one that counts.
int MbedTlsServer::Session::recvCb(void* self, unsigned char* buf, size_t len, uint32_t) {
    Session* s = static_cast<Session*>(self);
    const int32_t n = s->io->recv(buf, len, s->timeoutMs);
    if (n > 0) return static_cast<int>(n);
    if (n == 0) return MBEDTLS_ERR_SSL_TIMEOUT;
    return 0;   // the peer closed: mbedTLS reports MBEDTLS_ERR_SSL_CONN_EOF
}

bool MbedTlsServer::Session::handshake(TlsIo& conn, uint32_t timeout) {
    close();
    io = &conn;
    timeoutMs = timeout;   // per read: the handshake has two or three round trips
    if (mbedtls_ssl_setup(&ssl, &server->conf_) != 0) {   // allocates the record buffers
        mbedtls_ssl_free(&ssl);
        mbedtls_ssl_init(&ssl);
        ++server->failures_;
        return false;
    }
    setUp = true;
    mbedtls_ssl_set_bio(&ssl, this, sendCb, nullptr, recvCb);
    int r;
    do {
        r = mbedtls_ssl_handshake(&ssl);
    } while (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE);
    if (r != 0) {
        ++server->failures_;
        close();
        return false;
    }
    open = true;
    ++server->handshakes_;
    return true;
}

int32_t MbedTlsServer::Session::read(uint8_t* buf, size_t len, uint32_t timeout) {
    if (!open) return -1;
    timeoutMs = timeout;
    for (;;) {
        const int r = mbedtls_ssl_read(&ssl, buf, len);
        if (r > 0) return r;
        if (r == MBEDTLS_ERR_SSL_TIMEOUT) return 0;
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        open = false;   // close_notify, the connection gone, or an error
        return -1;
    }
}

int32_t MbedTlsServer::Session::write(const uint8_t* buf, size_t len, uint32_t timeout) {
    if (!open) return -1;
    timeoutMs = timeout;
    size_t done = 0;
    while (done < len) {
        const int r = mbedtls_ssl_write(&ssl, buf + done, len - done);
        if (r > 0) {
            done += static_cast<size_t>(r);
        } else if (r != MBEDTLS_ERR_SSL_WANT_WRITE && r != MBEDTLS_ERR_SSL_WANT_READ) {
            open = false;
            return -1;
        }
    }
    return static_cast<int32_t>(len);
}

void MbedTlsServer::Session::close() {
    if (open) {
        timeoutMs = 1000;
        mbedtls_ssl_close_notify(&ssl);
        open = false;
    }
    if (setUp) {
        mbedtls_ssl_free(&ssl);   // the record buffers go back to the heap
        mbedtls_ssl_init(&ssl);
        setUp = false;
    }
}
