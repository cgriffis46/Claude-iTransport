#include "MqttClient.h"
#include <cstring>

namespace {

enum Type : uint8_t {
    CONNECT = 1, CONNACK = 2, PUBLISH = 3, PUBACK = 4, SUBSCRIBE = 8, SUBACK = 9,
    UNSUBSCRIBE = 10, UNSUBACK = 11, PINGREQ = 12, PINGRESP = 13, DISCONNECT = 14
};

// One piece of a packet after the fixed header. str: a UTF-8 string or
// binary field, which goes out with a 2-byte length in front.
struct Part {
    const uint8_t* p;
    size_t         len;
    bool           str;
};

Part raw(const void* p, size_t len) { return Part{static_cast<const uint8_t*>(p), len, false}; }
Part str(const char* s) { return Part{reinterpret_cast<const uint8_t*>(s), s ? std::strlen(s) : 0, true}; }
Part bin(const uint8_t* p, size_t len) { return Part{p, len, true}; }

size_t varintLen(size_t n) { return n < 128 ? 1 : n < 16384 ? 2 : n < 2097152 ? 3 : 4; }

// The whole packet into dst; its length, or 0 if it doesn't fit (or a
// string field is over 65535). needed: what it would take.
size_t build(uint8_t* dst, size_t cap, uint8_t header, const Part* parts, int n, size_t* needed = nullptr) {
    size_t rem = 0;
    for (int i = 0; i < n; ++i) {
        if (parts[i].str && parts[i].len > 0xFFFF) return 0;
        rem += parts[i].len + (parts[i].str ? 2 : 0);
    }
    const size_t total = 1 + varintLen(rem) + rem;
    if (needed) *needed = total;
    if (rem > 268435455u || total > cap) return 0;
    uint8_t* o = dst;
    *o++ = header;
    size_t v = rem;
    do {
        uint8_t b = static_cast<uint8_t>(v % 128);
        v /= 128;
        if (v) b |= 0x80;
        *o++ = b;
    } while (v);
    for (int i = 0; i < n; ++i) {
        if (parts[i].str) {
            *o++ = static_cast<uint8_t>(parts[i].len >> 8);
            *o++ = static_cast<uint8_t>(parts[i].len);
        }
        if (parts[i].len) std::memcpy(o, parts[i].p, parts[i].len);
        o += parts[i].len;
    }
    return total;
}

uint16_t be16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }

// A topic to publish to: not empty, no wildcards.
bool validTopic(const char* t) {
    if (t == nullptr || *t == 0 || std::strlen(t) > 0xFFFF) return false;
    return std::strpbrk(t, "+#") == nullptr;
}

// A filter: '+' only as a whole level, '#' only as the whole last level.
bool validFilter(const char* f) {
    if (f == nullptr || *f == 0 || std::strlen(f) > MqttClient::kMaxFilter) return false;
    for (const char* p = f; *p; ++p) {
        const bool levelStart = p == f || p[-1] == '/';
        const bool levelEnd = p[1] == 0 || p[1] == '/';
        if (*p == '+' && !(levelStart && levelEnd)) return false;
        if (*p == '#' && !(levelStart && p[1] == 0)) return false;
    }
    return true;
}

uint32_t remaining(uint32_t now, uint32_t t0, uint32_t ms) {
    const uint32_t gone = now - t0;
    return gone >= ms ? 0 : ms - gone;
}

} // namespace

MqttClient::MqttClient(const Buffers& b, Listener& listener)
    : rx_(b.rx), rxSize_(b.rxSize), tx_(b.tx), txSize_(b.txSize), flight_(b.flight),
      slotSize_(b.flightSize / kMaxInFlight), listener_(listener) {}

// ---- connection ----

void MqttClient::start(const Options& o, uint32_t nowMs) {
    opt_ = o;
    state_ = State::Connecting;
    error_ = Error::None;
    refused_ = 0;
    txLen_ = 0;
    lenBytes_ = 0;
    haveHdr_ = false;
    inBody_ = false;
    pingOut_ = false;
    resendPending_ = resubPending_ = false;
    startedAt_ = lastTx_ = lastRx_ = nowMs;
    for (Slot& s : slots_) s.sent = false;

    static const char kProto[] = "MQTT";
    uint8_t flags = 0;
    if (o.cleanSession) flags |= 0x02;
    if (o.willTopic) flags |= static_cast<uint8_t>(0x04 | ((o.willQos & 1) << 3) | (o.willRetain ? 0x20 : 0));
    if (o.username) flags |= 0x80;
    if (o.username && o.password) flags |= 0x40;   // a password without a user name isn't allowed in 3.1.1
    const uint8_t fixed[4] = {4 /* 3.1.1 */, flags, static_cast<uint8_t>(o.keepAliveSec >> 8),
                              static_cast<uint8_t>(o.keepAliveSec)};
    Part parts[7];
    int n = 0;
    parts[n++] = str(kProto);
    parts[n++] = raw(fixed, 4);
    parts[n++] = str(o.clientId ? o.clientId : "");
    if (o.willTopic) {
        parts[n++] = str(o.willTopic);
        parts[n++] = bin(o.willPayload, o.willLen);
    }
    if (o.username) parts[n++] = str(o.username);
    if (o.username && o.password) parts[n++] = str(o.password);
    const size_t len = build(tx_, txSize_, CONNECT << 4, parts, n);
    if (len == 0) { drop(Error::TooBig); return; }
    txLen_ = len;
}

void MqttClient::connectionLost() {
    state_ = State::Disconnected;
    txLen_ = 0;
    lenBytes_ = 0;
    haveHdr_ = false;
    inBody_ = false;
    pingOut_ = false;
    for (Slot& s : slots_) s.sent = false;
}

void MqttClient::disconnect() {
    if (state_ != State::Connected) return;
    const uint8_t pkt[2] = {DISCONNECT << 4, 0};
    queue(pkt, 2);
}

// Out of the session: the owner closes the TCP connection.
void MqttClient::drop(Error why) {
    connectionLost();
    error_ = why;
    listener_.onDisconnected(why);
}

// ---- output ----

bool MqttClient::queue(const uint8_t* packet, size_t len) {
    if (len > txSize_ - txLen_) return false;
    std::memcpy(tx_ + txLen_, packet, len);
    txLen_ += len;
    return true;
}

void MqttClient::consume(size_t n) {
    if (n > txLen_) n = txLen_;
    std::memmove(tx_, tx_ + n, txLen_ - n);
    txLen_ -= n;
}

uint16_t MqttClient::nextId() {
    for (;;) {
        if (++lastId_ == 0) lastId_ = 1;
        bool inUse = false;
        for (const Slot& s : slots_) inUse |= s.used && s.id == lastId_;
        if (!inUse) return lastId_;
    }
}

uint8_t MqttClient::inFlight() const {
    uint8_t n = 0;
    for (const Slot& s : slots_) n += s.used;
    return n;
}

// ---- requests ----

bool MqttClient::publish(const char* topic, const uint8_t* payload, size_t len, uint8_t qos, bool retain,
                         uint32_t nowMs, uint8_t* slot) {
    if (!validTopic(topic) || qos > 1 || (len && payload == nullptr)) { error_ = Error::BadArgument; return false; }
    if (qos == 0) {
        if (state_ != State::Connected) { error_ = Error::NotConnected; return false; }
        const Part parts[2] = {str(topic), raw(payload, len)};
        size_t needed = 0;
        const size_t n = build(tx_ + txLen_, txSize_ - txLen_, static_cast<uint8_t>((PUBLISH << 4) | (retain ? 1 : 0)),
                               parts, 2, &needed);
        if (n == 0) { error_ = needed > txSize_ ? Error::TooBig : Error::Busy; return false; }
        txLen_ += n;
        lastTx_ = nowMs;
        return true;
    }
    uint8_t i = 0;
    while (i < kMaxInFlight && slots_[i].used) ++i;
    if (i == kMaxInFlight) { error_ = Error::Busy; return false; }
    const uint16_t id = nextId();
    const uint8_t idBytes[2] = {static_cast<uint8_t>(id >> 8), static_cast<uint8_t>(id)};
    const Part parts[3] = {str(topic), raw(idBytes, 2), raw(payload, len)};
    const size_t n = build(slotData(i), slotSize_ > txSize_ ? txSize_ : slotSize_,
                           static_cast<uint8_t>((PUBLISH << 4) | (1 << 1) | (retain ? 1 : 0)), parts, 3);
    if (n == 0) { error_ = Error::TooBig; return false; }
    slots_[i] = Slot{true, false, id, static_cast<uint16_t>(n)};
    everSent_[i] = false;
    if (slot) *slot = i;
    if (state_ == State::Connected) {
        resendPending_ = true;
        resendInFlight();
        lastTx_ = nowMs;
    }
    return true;
}

bool MqttClient::subscribe(const char* filter, uint8_t qos, uint16_t& id) {
    if (!validFilter(filter) || qos > 1) { error_ = Error::BadArgument; return false; }
    int at = -1, free = -1;
    for (int i = 0; i < kMaxSubscriptions; ++i) {
        if (subs_[i].used && std::strcmp(subs_[i].filter, filter) == 0) at = i;
        if (!subs_[i].used && free < 0) free = i;
    }
    if (at < 0 && free < 0) { error_ = Error::Busy; return false; }
    id = nextId();
    if (state_ == State::Connected) {
        const uint8_t idBytes[2] = {static_cast<uint8_t>(id >> 8), static_cast<uint8_t>(id)};
        const Part parts[3] = {raw(idBytes, 2), str(filter), raw(&qos, 1)};
        const size_t n = build(tx_ + txLen_, txSize_ - txLen_, (SUBSCRIBE << 4) | 0x02, parts, 3);
        if (n == 0) { error_ = Error::Busy; return false; }
        txLen_ += n;
    }
    Sub& s = subs_[at >= 0 ? at : free];
    s.used = true;
    s.qos = qos;
    std::strcpy(s.filter, filter);
    return true;
}

bool MqttClient::unsubscribe(const char* filter, uint16_t& id) {
    if (!validFilter(filter)) { error_ = Error::BadArgument; return false; }
    if (state_ != State::Connected) { error_ = Error::NotConnected; return false; }
    id = nextId();
    const uint8_t idBytes[2] = {static_cast<uint8_t>(id >> 8), static_cast<uint8_t>(id)};
    const Part parts[2] = {raw(idBytes, 2), str(filter)};
    const size_t n = build(tx_ + txLen_, txSize_ - txLen_, (UNSUBSCRIBE << 4) | 0x02, parts, 2);
    if (n == 0) { error_ = Error::Busy; return false; }
    txLen_ += n;
    for (Sub& s : subs_) {
        if (s.used && std::strcmp(s.filter, filter) == 0) s.used = false;
    }
    return true;
}

// After CONNACK, and whenever output frees up while some are left:
// every unacknowledged QoS 1 message, DUP if it went out before.
void MqttClient::resendInFlight() {
    if (!resendPending_) return;
    for (uint8_t i = 0; i < kMaxInFlight; ++i) {
        Slot& s = slots_[i];
        if (!s.used || s.sent) continue;
        if (everSent_[i]) slotData(i)[0] |= 0x08;   // DUP
        if (!queue(slotData(i), s.len)) return;      // the rest when there is room
        s.sent = true;
        everSent_[i] = true;
    }
    resendPending_ = false;
}

// Every remembered filter, in one SUBSCRIBE.
void MqttClient::resubscribe() {
    if (!resubPending_) return;
    Part parts[1 + 2 * kMaxSubscriptions];
    int n = 0;
    const uint16_t id = nextId();
    const uint8_t idBytes[2] = {static_cast<uint8_t>(id >> 8), static_cast<uint8_t>(id)};
    parts[n++] = raw(idBytes, 2);
    for (const Sub& s : subs_) {
        if (!s.used) continue;
        parts[n++] = str(s.filter);
        parts[n++] = raw(&s.qos, 1);
    }
    if (n > 1) {
        const size_t m = build(tx_ + txLen_, txSize_ - txLen_, (SUBSCRIBE << 4) | 0x02, parts, n);
        if (m == 0) return;   // no room yet: poll() tries again
        txLen_ += m;
    }
    resubPending_ = false;
}

void MqttClient::sendPing(uint32_t nowMs) {
    const uint8_t pkt[2] = {PINGREQ << 4, 0};
    if (!queue(pkt, 2)) return;
    pingOut_ = true;
    pingAt_ = nowMs;
    lastTx_ = nowMs;
}

// ---- timers ----

void MqttClient::poll(uint32_t nowMs) {
    if (state_ == State::Connecting) {
        if (nowMs - startedAt_ >= kConnackTimeoutMs) drop(Error::Timeout);
        return;
    }
    if (state_ != State::Connected) return;
    resubscribe();
    resendInFlight();
    if (opt_.keepAliveSec == 0) return;
    const uint32_t k = opt_.keepAliveSec * 1000u;
    if (pingOut_) {
        // No PINGRESP within half the keepalive (at least 3 s): gone.
        const uint32_t limit = k / 2 > 3000 ? k / 2 : 3000;
        if (nowMs - pingAt_ >= limit) drop(Error::Timeout);
        return;
    }
    // Quiet either way for a keepalive period: check it's still there.
    if (nowMs - lastTx_ >= k || nowMs - lastRx_ >= k) sendPing(nowMs);
}

uint32_t MqttClient::nextWakeMs(uint32_t nowMs) const {
    if (state_ == State::Connecting) return remaining(nowMs, startedAt_, kConnackTimeoutMs);
    if (state_ != State::Connected) return 0xFFFFFFFFu;
    if (resendPending_ || resubPending_) return 10;   // waiting for room in the output
    if (opt_.keepAliveSec == 0) return 0xFFFFFFFFu;
    const uint32_t k = opt_.keepAliveSec * 1000u;
    if (pingOut_) return remaining(nowMs, pingAt_, k / 2 > 3000 ? k / 2 : 3000);
    const uint32_t a = remaining(nowMs, lastTx_, k), b = remaining(nowMs, lastRx_, k);
    return a < b ? a : b;
}

// ---- input ----

void MqttClient::receive(const uint8_t* data, size_t len, uint32_t nowMs) {
    for (size_t i = 0; i < len && state_ != State::Disconnected; ++i) {
        const uint8_t c = data[i];
        if (!inBody_ && lenBytes_ == 0 && !haveHdr_) {
            hdr_ = c;
            haveHdr_ = true;
            remaining_ = 0;
            mult_ = 1;
            continue;
        }
        if (!inBody_) {
            remaining_ += (c & 0x7Fu) * mult_;
            mult_ *= 128;
            ++lenBytes_;
            if (c & 0x80) {
                if (lenBytes_ == 4) { drop(Error::Protocol); return; }   // a fifth length byte
                continue;
            }
            lenBytes_ = 0;
            haveHdr_ = false;
            if (remaining_ == 0) {
                dispatch(hdr_, rx_, 0, nowMs);
                continue;
            }
            inBody_ = true;
            got_ = 0;
            skipping_ = remaining_ > rxSize_;
            continue;
        }
        // The body: copy what's here in one go.
        size_t take = len - i;
        if (take > remaining_ - got_) take = remaining_ - got_;
        if (!skipping_) std::memcpy(rx_ + got_, data + i, take);
        got_ += static_cast<uint32_t>(take);
        i += take - 1;
        if (got_ == remaining_) {
            inBody_ = false;
            if (skipping_) {
                ++oversized_;
                lastRx_ = nowMs;
            } else {
                dispatch(hdr_, rx_, remaining_, nowMs);
            }
        }
    }
}

void MqttClient::dispatch(uint8_t hdr, uint8_t* body, size_t len, uint32_t nowMs) {
    lastRx_ = nowMs;
    const uint8_t type = hdr >> 4;
    if (state_ == State::Connecting && type != CONNACK) { drop(Error::Protocol); return; }

    switch (type) {
    case CONNACK:
        if (state_ != State::Connecting || len != 2) { drop(Error::Protocol); return; }
        if (body[1] != 0) {
            refused_ = body[1];
            drop(Error::Refused);
            return;
        }
        state_ = State::Connected;
        resubPending_ = (body[0] & 1) == 0;   // the broker forgot our subscriptions
        resendPending_ = true;
        listener_.onConnected((body[0] & 1) != 0);
        resubscribe();
        resendInFlight();
        return;

    case PUBLISH: {
        const uint8_t qos = (hdr >> 1) & 3;
        if (qos > 1 || len < 2) { drop(Error::Protocol); return; }   // we never ask for QoS 2
        const size_t tlen = be16(body);
        const size_t head = 2 + tlen + (qos ? 2 : 0);
        if (tlen == 0 || head > len) { drop(Error::Protocol); return; }
        const uint16_t id = qos ? be16(body + 2 + tlen) : 0;
        // The topic, NUL-terminated in place: moved over its own length.
        std::memmove(body, body + 2, tlen);
        body[tlen] = 0;
        if (std::memchr(body, 0, tlen) != nullptr) { drop(Error::Protocol); return; }
        const bool taken = listener_.onMessage(reinterpret_cast<const char*>(body), body + head, len - head, qos,
                                               (hdr & 1) != 0);
        if (qos == 1 && taken) {
            const uint8_t ack[4] = {PUBACK << 4, 2, static_cast<uint8_t>(id >> 8), static_cast<uint8_t>(id)};
            if (queue(ack, 4)) lastTx_ = nowMs;
        }
        return;
    }

    case PUBACK: {
        if (len != 2) { drop(Error::Protocol); return; }
        const uint16_t id = be16(body);
        for (uint8_t i = 0; i < kMaxInFlight; ++i) {
            if (slots_[i].used && slots_[i].id == id) {
                slots_[i].used = false;
                listener_.onPublished(i);
                break;
            }
        }
        return;
    }

    case SUBACK: {
        if (len < 3) { drop(Error::Protocol); return; }
        bool ok = true;
        for (size_t i = 2; i < len; ++i) ok &= body[i] != 0x80;
        listener_.onSubscribed(be16(body), ok);
        return;
    }

    case UNSUBACK:
        if (len != 2) { drop(Error::Protocol); return; }
        listener_.onUnsubscribed(be16(body));
        return;

    case PINGRESP:
        pingOut_ = false;
        return;

    default:
        drop(Error::Protocol);   // nothing else is for a client
        return;
    }
}

// ---- topics ----

bool MqttClient::topicMatches(const char* f, const char* t) {
    if (f == nullptr || t == nullptr) return false;
    if (*t == '$' && (*f == '+' || *f == '#')) return false;   // $SYS and friends only by name
    for (;;) {
        if (*f == '#') return true;
        if (*f == '+') {
            ++f;
            while (*t && *t != '/') ++t;
        } else if (*f && *t && *f != '/' && *t != '/') {
            if (*f++ != *t++) return false;
            continue;
        }
        // At the end of a level in one or both.
        if (*f == 0) return *t == 0;
        if (*t == 0) return std::strcmp(f, "/#") == 0;   // "a/#" matches "a" too
        if (*f != '/' || *t != '/') return false;
        ++f;
        ++t;
    }
}
