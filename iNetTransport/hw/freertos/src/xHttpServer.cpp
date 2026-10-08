#include "xHttpServer.h"
#include <new>

xHttpServer::xHttpServer(xNetInterface& net, const Config& cfg) : net_(net), cfg_(cfg) {}

xHttpServer::~xHttpServer() {
    if (clients_) {
        for (uint8_t i = 0; i < cfg_.maxClients; ++i) {
            Client& c = clients_[i];
            if (c.tokens) vQueueDelete(c.tokens);
            vPortFree(c.arena);
            c.~Client();
        }
        vPortFree(clients_);
    }
    if (slots_) vSemaphoreDelete(slots_);
}

bool xHttpServer::begin() {
    if (clients_) return true;
    if (cfg_.maxClients == 0 || cfg_.requestBytes < 64 || cfg_.tokenQueueDepth == 0) return false;
    slots_ = xSemaphoreCreateCounting(cfg_.maxClients, cfg_.maxClients);
    clients_ = static_cast<Client*>(pvPortMalloc(sizeof(Client) * cfg_.maxClients));
    if (slots_ == nullptr || clients_ == nullptr) {
        vPortFree(clients_);
        clients_ = nullptr;
        return false;
    }
    bool ok = true;
    for (uint8_t i = 0; i < cfg_.maxClients; ++i) {
        uint8_t* arena = static_cast<uint8_t*>(pvPortMalloc(cfg_.requestBytes));
        Client* c = new (&clients_[i]) Client(*this, net_, arena, cfg_.requestBytes, cfg_.maxRequestsPerConnection);
        c->tokens = xQueueCreate(cfg_.tokenQueueDepth, sizeof(HttpToken));
        ok &= arena != nullptr && c->tokens != nullptr;
    }
    return ok;   // on false the destructor frees what was allocated
}

xHttpServer::Stats xHttpServer::stats() const {
    Stats s;
    s.accepted = stAccepted_.load();
    s.requests = stRequests_.load();
    s.errors = stErrors_.load();
    s.timeouts = stTimeouts_.load();
    s.spawnFailures = stSpawnFailures_.load();
    s.tlsFailures = stTlsFailures_.load();
    s.active = active_.load();
    return s;
}

void xHttpServer::stop() {
    stopping_ = true;
}

xHttpServer::Client* xHttpServer::freeClient() {
    for (uint8_t i = 0; i < cfg_.maxClients; ++i) {
        if (!clients_[i].busy) return &clients_[i];
    }
    return nullptr;   // can't happen: slots_ counts the free ones
}

// ---- the daemon thread ----

void xHttpServer::run() {
    configASSERT(clients_ != nullptr);   // begin() first
    while (!stopping_) {
        // A free client slot. While every one is busy, nothing listens
        // and new connections are refused.
        if (xSemaphoreTake(slots_, xNetInterface::toTicks(500)) != pdPASS) continue;
        Client* c = freeClient();
        bool handed = false;
        if (c != nullptr && net_.waitAddress(500)) {
            if (c->sock.listen(cfg_.port, 1000)) {
                while (!stopping_ && !c->sock.accept(500)) {
                }
                if (!stopping_) {
                    stAccepted_.fetch_add(1);
                    c->busy = true;
                    active_.fetch_add(1);
                    // The client's thread: it owns the socket from here on.
                    if (xTaskCreate(clientThread, "http", static_cast<configSTACK_DEPTH_TYPE>(cfg_.stackWords), c,
                                    cfg_.priority, nullptr) == pdPASS) {
                        handed = true;
                    } else {
                        stSpawnFailures_.fetch_add(1);
                        refuse(*c, 503);
                        active_.fetch_sub(1);
                        c->busy = false;
                    }
                }
            } else {
                vTaskDelay(xNetInterface::toTicks(200));   // no socket free: try again shortly
            }
        }
        if (!handed) {
            if (c) c->sock.stop(1000);
            xSemaphoreGive(slots_);
        }
    }
    // Every client thread sees stopping_ within half a second, closes its
    // connection and gives its slot back.
    uint8_t got = 0;
    for (uint8_t i = 0; i < cfg_.maxClients; ++i) {
        if (xSemaphoreTake(slots_, xNetInterface::toTicks(10000)) == pdPASS) ++got;
    }
    for (uint8_t i = 0; i < got; ++i) xSemaphoreGive(slots_);
}

void xHttpServer::refuse(Client& c, uint16_t code) {
    if (cfg_.tls) return;   // a 503 would need a TLS handshake first: just close
    HttpResponse res(c);
    res.reset(true, false, false);
    res.sendStatus(code);
    c.sock.flush(1000);
}

// ---- a client's thread ----

void xHttpServer::clientThread(void* arg) {
    Client* c = static_cast<Client*>(arg);
    xHttpServer* s = c->server;
    c->serve();
    s->active_.fetch_sub(1);
    c->busy = false;
    xSemaphoreGive(s->slots_);
    vTaskDelete(nullptr);   // a FreeRTOS task never returns
}

bool xHttpServer::Client::write(const void* data, size_t len) {
    if (tls) return len == 0 || tls->write(static_cast<const uint8_t*>(data), len, 5000) == static_cast<int32_t>(len);
    const uint8_t* p = static_cast<const uint8_t*>(data);
    while (len) {
        const int32_t w = sock.write(p, len, 5000);
        if (w <= 0) return false;   // closed, or stalled for 5 s
        p += w;
        len -= static_cast<size_t>(w);
    }
    return true;
}

int32_t xHttpServer::Client::recv(uint8_t* buf, size_t len, uint32_t timeoutMs) {
    return sock.read(buf, len, timeoutMs);
}

int32_t xHttpServer::Client::send(const uint8_t* buf, size_t len, uint32_t timeoutMs) {
    return sock.write(buf, len, timeoutMs);
}

bool xHttpServer::Client::put(const HttpToken& t) {
    return xQueueSend(tokens, &t, 0) == pdPASS;
}

// The token stream's far end: every token on the queue, through the
// state machine. Handlers run from here.
void xHttpServer::Client::drain() {
    HttpToken t;
    const uint32_t reqs = conn.requests(), errs = conn.errors();
    while (xQueueReceive(tokens, &t, 0) == pdPASS) conn.onToken(t);
    server->stRequests_.fetch_add(conn.requests() - reqs);
    server->stErrors_.fetch_add(conn.errors() - errs);
}

void xHttpServer::Client::serve() {
    const Config& cfg = server->cfg_;
    lexer.reset();
    conn.reset();
    xQueueReset(tokens);
    if (cfg.tls) {
        // HTTPS: the handshake first. Its ECC is the slow part (most of a
        // second on a Cortex-M3), done once per browser thanks to tickets.
        tls = cfg.tls->acquire();
        if (tls == nullptr || !tls->handshake(*this, cfg.requestTimeoutMs)) {
            server->stTlsFailures_.fetch_add(1);
            if (tls) cfg.tls->release(tls);
            tls = nullptr;
            sock.stop(1000);
            return;
        }
    }
    conn.setSecure(tls != nullptr);
    bool inRequest = false;
    TickType_t since = xTaskGetTickCount();   // start of the request, or of the idle wait

    while (!conn.done() && !server->stopping_) {
        const bool mid = lexer.midMessage() || conn.midRequest();
        if (mid != inRequest) {
            inRequest = mid;
            since = xTaskGetTickCount();
        }
        const TickType_t limit = xNetInterface::toTicks(inRequest ? cfg.requestTimeoutMs : cfg.idleTimeoutMs);
        const TickType_t elapsed = xTaskGetTickCount() - since;
        if (elapsed >= limit) {
            const uint32_t reqs = conn.requests();
            conn.timeout();   // 408 if a request was under way
            server->stRequests_.fetch_add(conn.requests() - reqs);
            server->stTimeouts_.fetch_add(1);
            break;
        }
        TickType_t wait = limit - elapsed;
        if (wait > xNetInterface::toTicks(500)) wait = xNetInterface::toTicks(500);   // to see stop()

        const uint32_t waitMs = wait * portTICK_PERIOD_MS;
        const int32_t n = tls ? tls->read(readBuf, sizeof readBuf, waitMs) : sock.read(readBuf, sizeof readBuf, waitMs);
        if (n < 0) break;   // the client closed
        size_t used = 0;
        while ((used < static_cast<size_t>(n) || lexer.pending()) && !conn.done()) {
            used += lexer.feed(readBuf + used, static_cast<size_t>(n) - used, *this);
            drain();
        }
    }
    if (tls) {
        cfg.tls->release(tls);   // close_notify, and its buffers back to the heap
        tls = nullptr;
    }
    sock.flush(1000);
    sock.stop(1000);
}
