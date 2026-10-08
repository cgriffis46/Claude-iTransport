#include "xMqttClient.h"
#include <cstring>
#include <new>

xMqttClient::xMqttClient(xNetInterface& net, const Config& cfg)
    : net_(net), cfg_(cfg), client_(net) {}

xMqttClient::~xMqttClient() {
    if (core_) core_->~MqttClient();
    vPortFree(rx_);
    vPortFree(tx_);
    vPortFree(flight_);
    vPortFree(scratch_);
    if (inbox_) vMessageBufferDelete(inbox_);
    const SemaphoreHandle_t sems[] = {session_, requestMutex_, requestDone_, recvMutex_};
    for (SemaphoreHandle_t s : sems) {
        if (s) vSemaphoreDelete(s);
    }
    if (events_) vEventGroupDelete(events_);
}

uint32_t xMqttClient::nowMs() {
    return static_cast<uint32_t>(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

bool xMqttClient::begin() {
    if (core_) return true;
    if (cfg_.host == nullptr || *cfg_.host == 0 || cfg_.maxPacket < 64) return false;
    rx_ = static_cast<uint8_t*>(pvPortMalloc(cfg_.maxPacket));
    tx_ = static_cast<uint8_t*>(pvPortMalloc(cfg_.maxPacket));
    flight_ = static_cast<uint8_t*>(pvPortMalloc(cfg_.inFlightBytes));
    scratch_ = static_cast<uint8_t*>(pvPortMalloc(cfg_.maxPacket + 4));
    inbox_ = xMessageBufferCreate(cfg_.inboxBytes);
    session_ = xSemaphoreCreateRecursiveMutex();
    requestMutex_ = xSemaphoreCreateMutex();
    requestDone_ = xSemaphoreCreateBinary();
    recvMutex_ = xSemaphoreCreateMutex();
    events_ = xEventGroupCreate();
    bool ok = rx_ && tx_ && flight_ && scratch_ && inbox_ && session_ && requestMutex_ && requestDone_ &&
              recvMutex_ && events_;
    if (!ok) return false;
    core_ = new (coreMem_) MqttClient(
        MqttClient::Buffers{rx_, cfg_.maxPacket, tx_, cfg_.maxPacket, flight_, cfg_.inFlightBytes}, *this);
    backoffMs_ = cfg_.reconnectMinMs;
    return true;
}

bool xMqttClient::lock(uint32_t timeoutMs) {
    return xSemaphoreTakeRecursive(session_, xNetInterface::toTicks(timeoutMs)) == pdPASS;
}

void xMqttClient::unlock() {
    xSemaphoreGiveRecursive(session_);
}

// ---- the MQTT thread ----

void xMqttClient::run() {
    configASSERT(core_ != nullptr);   // begin() first
    thread_ = xTaskGetCurrentTaskHandle();
    while (!stopping_) {
        if (!tcpUp_) {
            // No network yet: wait for an address rather than spin.
            if (!net_.waitAddress(500)) continue;
            IpAddress ip;
            if (!net_.resolve(cfg_.host, ip, 5000)) { backoff(); continue; }
            bool up = false;
            if (lock(xNetInterface::kForever)) {
                up = client_.connect(ip, cfg_.port, cfg_.connectTimeoutMs);
                if (up) {
                    tcpUp_ = true;
                    core_->start(cfg_.session, nowMs());
                    flushLocked();
                }
                unlock();
            }
            if (!up) { backoff(); continue; }
        }

        // Sleep in read() until the broker sends something or a timer is due.
        uint32_t wait = 1000;   // at most a second, so stop() is seen
        if (lock(xNetInterface::kForever)) {
            const uint32_t w = core_->nextWakeMs(nowMs());
            if (w < wait) wait = w;
            unlock();
        }
        const int32_t n = client_.read(readBuf_, sizeof readBuf_, wait);

        bool lost = false;
        if (lock(xNetInterface::kForever)) {
            if (n > 0) core_->receive(readBuf_, static_cast<size_t>(n), nowMs());
            if (n < 0) {
                core_->connectionLost();   // the broker closed it
            } else {
                core_->poll(nowMs());
                flushLocked();
            }
            if (tcpUp_ && core_->state() == MqttClient::State::Disconnected) closeLocked();
            lost = !tcpUp_;
            unlock();
        }
        if (lost) backoff();
    }

    // A clean goodbye: the broker doesn't publish the will.
    if (lock(xNetInterface::kForever)) {
        if (tcpUp_) {
            core_->disconnect();
            flushLocked();
            client_.flush(1000);
            core_->connectionLost();
            closeLocked();
        }
        unlock();
    }
}

void xMqttClient::stop() {
    stopping_ = true;
    if (events_) xEventGroupSetBits(events_, kStop);
}

// Sleeps the backoff (cut short by stop()), and doubles it.
void xMqttClient::backoff() {
    xEventGroupWaitBits(events_, kStop, pdFALSE, pdFALSE, xNetInterface::toTicks(backoffMs_));
    backoffMs_ = backoffMs_ * 2 > cfg_.reconnectMaxMs ? cfg_.reconnectMaxMs : backoffMs_ * 2;
}

void xMqttClient::flushLocked() {
    while (tcpUp_) {
        size_t n = 0;
        const uint8_t* out = core_->output(n);
        if (n == 0) return;
        const int32_t w = client_.write(out, n, 5000);
        if (w <= 0) {
            // Closed, or stalled for 5 s. The MQTT thread closes the
            // socket (it may be in read() on it): it sees Disconnected
            // as soon as read() returns.
            core_->connectionLost();
            return;
        }
        core_->consume(static_cast<size_t>(w));
    }
}

void xMqttClient::closeLocked() {
    if (!tcpUp_) return;
    tcpUp_ = false;
    client_.stop(1000);
    xEventGroupClearBits(events_, kConnected);
    stDisconnects_.fetch_add(1, std::memory_order_relaxed);
}

// ---- MqttClient::Listener ----

void xMqttClient::onConnected(bool) {
    backoffMs_ = cfg_.reconnectMinMs;
    stConnects_.fetch_add(1, std::memory_order_relaxed);
    xEventGroupSetBits(events_, kConnected);
}

void xMqttClient::onDisconnected(MqttClient::Error why) {
    if (why == MqttClient::Error::Refused) stRefused_.fetch_add(1, std::memory_order_relaxed);
    // run() sees the state and closes the connection.
}

bool xMqttClient::onMessage(const char* topic, const uint8_t* payload, size_t len, uint8_t qos, bool retain) {
    if (cb_) {
        MqttMessage m;
        m.topic = topic;
        m.payload = payload;
        m.len = len;
        m.qos = qos;
        m.retain = retain;
        const bool taken = cb_(m, cbCtx_);
        if (taken) stReceived_.fetch_add(1, std::memory_order_relaxed);
        else stDropped_.fetch_add(1, std::memory_order_relaxed);
        return taken;
    }
    // Framed for the inbox: topic length, flags, topic, NUL, payload.
    const size_t tlen = std::strlen(topic);
    const size_t total = 3 + tlen + 1 + len;
    if (total > cfg_.maxPacket + 4) { stDropped_.fetch_add(1, std::memory_order_relaxed); return false; }
    scratch_[0] = static_cast<uint8_t>(tlen >> 8);
    scratch_[1] = static_cast<uint8_t>(tlen);
    scratch_[2] = static_cast<uint8_t>((qos & 3) | (retain ? 0x80 : 0));
    std::memcpy(scratch_ + 3, topic, tlen + 1);
    if (len) std::memcpy(scratch_ + 3 + tlen + 1, payload, len);
    // Never waits: a full inbox means the reader is behind. A QoS 1
    // message then isn't acknowledged, and comes again after a reconnect.
    if (xMessageBufferSend(inbox_, scratch_, total, 0) != total) {
        stDropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    stReceived_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void xMqttClient::onPublished(uint8_t slot) {
    if (slot >= MqttClient::kMaxInFlight) return;
    ackedGen_[slot] = sentGen_[slot];
    xEventGroupSetBits(events_, kAck << slot);
}

void xMqttClient::onSubscribed(uint16_t id, bool ok) {
    if (id != pendingId_ || id == 0) return;   // a re-subscribe after reconnecting, say
    pendingOk_ = ok;
    pendingId_ = 0;
    xSemaphoreGive(requestDone_);
}

void xMqttClient::onUnsubscribed(uint16_t id) {
    onSubscribed(id, true);
}

// ---- the user's side ----

bool xMqttClient::connected() const {
    return events_ != nullptr && (xEventGroupGetBits(events_) & kConnected) != 0;
}

bool xMqttClient::waitConnected(uint32_t timeoutMs) {
    if (events_ == nullptr) return false;
    return (xEventGroupWaitBits(events_, kConnected, pdFALSE, pdFALSE, xNetInterface::toTicks(timeoutMs)) & kConnected) != 0;
}

bool xMqttClient::publish(const char* topic, const void* payload, size_t len, uint8_t qos, bool retain,
                          uint32_t timeoutMs) {
    if (core_ == nullptr || (len && payload == nullptr)) return false;
    if (!lock(timeoutMs)) return false;
    uint8_t slot = 0;
    uint32_t gen = 0;
    const bool ok = core_->publish(topic, static_cast<const uint8_t*>(payload), len, qos, retain, nowMs(), &slot);
    if (ok && qos == 1) {
        gen = ++sentGen_[slot];
        // The bit may be left from the slot's last message, whose ack is
        // already in ackedGen_ for its own publisher to see.
        xEventGroupClearBits(events_, kAck << slot);
    }
    if (ok) flushLocked();
    unlock();
    if (!ok) return false;
    stPublished_.fetch_add(1, std::memory_order_relaxed);
    // The MQTT thread can't wait for an ack only it can receive.
    if (qos == 0 || timeoutMs == 0 || xTaskGetCurrentTaskHandle() == thread_) return true;
    return waitAck(slot, gen, timeoutMs);
}

// Whether the publish numbered gen in slot has been acknowledged,
// sleeping up to timeoutMs for it. A later publish into the same slot
// can clear the slot's bit between our look and our wait, so the wait
// is in slices of at most 50 ms: that case costs at most one slice.
bool xMqttClient::waitAck(uint8_t slot, uint32_t gen, uint32_t timeoutMs) {
    const TickType_t start = xTaskGetTickCount();
    const TickType_t total = xNetInterface::toTicks(timeoutMs);
    for (;;) {
        if (!lock(timeoutMs)) return false;
        const bool acked = static_cast<int32_t>(ackedGen_[slot] - gen) >= 0;
        unlock();
        if (acked) return true;
        const TickType_t used = xTaskGetTickCount() - start;
        if (used >= total) return false;
        TickType_t w = total - used;
        if (w > xNetInterface::toTicks(50)) w = xNetInterface::toTicks(50);
        xEventGroupWaitBits(events_, kAck << slot, pdFALSE, pdFALSE, w);
    }
}

bool xMqttClient::publish(const char* topic, const char* text, uint8_t qos, bool retain, uint32_t timeoutMs) {
    return publish(topic, text, text ? std::strlen(text) : 0, qos, retain, timeoutMs);
}

bool xMqttClient::subscribe(const char* filter, uint8_t qos, uint32_t timeoutMs) {
    if (core_ == nullptr) return false;
    if (xSemaphoreTake(requestMutex_, xNetInterface::toTicks(timeoutMs)) != pdPASS) return false;
    bool ok = false, online = false;
    if (lock(timeoutMs)) {
        uint16_t id = 0;
        ok = core_->subscribe(filter, qos, id);
        online = ok && core_->state() == MqttClient::State::Connected;
        if (online) {
            xSemaphoreTake(requestDone_, 0);
            pendingId_ = id;
            flushLocked();
        }
        unlock();
    }
    if (online) ok = xSemaphoreTake(requestDone_, xNetInterface::toTicks(timeoutMs)) == pdPASS && pendingOk_;
    xSemaphoreGive(requestMutex_);
    return ok;
}

bool xMqttClient::unsubscribe(const char* filter, uint32_t timeoutMs) {
    if (core_ == nullptr) return false;
    if (xSemaphoreTake(requestMutex_, xNetInterface::toTicks(timeoutMs)) != pdPASS) return false;
    bool ok = false;
    if (lock(timeoutMs)) {
        uint16_t id = 0;
        ok = core_->unsubscribe(filter, id);
        if (ok) {
            xSemaphoreTake(requestDone_, 0);
            pendingId_ = id;
            flushLocked();
        }
        unlock();
    }
    if (ok) ok = xSemaphoreTake(requestDone_, xNetInterface::toTicks(timeoutMs)) == pdPASS;
    xSemaphoreGive(requestMutex_);
    return ok;
}

bool xMqttClient::receive(uint8_t* buf, size_t cap, MqttMessage& out, uint32_t timeoutMs) {
    if (inbox_ == nullptr || buf == nullptr || cap < cfg_.maxPacket + 4) return false;
    if (xSemaphoreTake(recvMutex_, xNetInterface::toTicks(timeoutMs)) != pdPASS) return false;
    const size_t n = xMessageBufferReceive(inbox_, buf, cap, xNetInterface::toTicks(timeoutMs));
    xSemaphoreGive(recvMutex_);
    if (n < 4) return false;
    const size_t tlen = static_cast<size_t>((buf[0] << 8) | buf[1]);
    if (3 + tlen + 1 > n) return false;
    out.topic = reinterpret_cast<const char*>(buf + 3);
    out.payload = buf + 3 + tlen + 1;
    out.len = n - (3 + tlen + 1);
    out.qos = buf[2] & 3;
    out.retain = (buf[2] & 0x80) != 0;
    return true;
}

xMqttClient::Stats xMqttClient::stats() const {
    Stats s;
    s.connects = stConnects_.load(std::memory_order_relaxed);
    s.disconnects = stDisconnects_.load(std::memory_order_relaxed);
    s.refused = stRefused_.load(std::memory_order_relaxed);
    s.published = stPublished_.load(std::memory_order_relaxed);
    s.received = stReceived_.load(std::memory_order_relaxed);
    s.dropped = stDropped_.load(std::memory_order_relaxed);
    return s;
}
