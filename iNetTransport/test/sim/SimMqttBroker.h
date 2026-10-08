// An MQTT 3.1.1 broker for the host tests, serving one client
// connection at a time: it decodes what the client sends (with its own
// decoder, independent of MqttClient's), answers CONNECT, SUBSCRIBE,
// UNSUBSCRIBE, PUBLISH (QoS 1) and PINGREQ, routes publishes to the
// client's own subscriptions, and records everything. deliver() plays
// another client publishing. Thread-safe.
#pragma once
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

class SimMqttBroker {
public:
    struct Msg {
        std::string topic;
        std::vector<uint8_t> payload;
        uint8_t qos = 0;
        bool retain = false, dup = false;
        uint16_t id = 0;
    };

    // ---- knobs ----
    uint8_t connackCode = 0;      // 5: not authorised, ...
    bool sessionPresent = false;  // claim to have kept the session (on reconnects with clean session off)
    bool ackPublishes = true;     // PUBACK QoS 1 publishes
    bool answerPings = true;
    bool silent = false;          // answer nothing at all

    // ---- what happened ----
    int connects = 0, pings = 0, disconnects = 0, subscribePackets = 0;
    std::string clientId, username, password, willTopic, willPayload;
    uint8_t connectFlags = 0;
    uint16_t keepAlive = 0;
    std::vector<Msg> published;                         // from the client
    std::vector<std::pair<std::string, uint8_t>> subs;  // the client's current subscriptions
    std::vector<uint16_t> pubacks;                      // the client's PUBACKs for our QoS 1 deliveries
    bool protocolError = false;

    // A new TCP connection: the decoder starts afresh. Subscriptions go
    // unless the session is persistent and the broker says it kept it.
    void newConnection() {
        std::lock_guard<std::recursive_mutex> g(m_);
        in_.clear();
        out_.clear();
        connected_ = false;
    }

    // Bytes from the client.
    void feed(const uint8_t* d, size_t n) {
        std::lock_guard<std::recursive_mutex> g(m_);
        in_.insert(in_.end(), d, d + n);
        for (;;) {
            size_t len = 0, hdrLen = 0;
            if (!complete(len, hdrLen)) break;
            const std::vector<uint8_t> pkt(in_.begin(), in_.begin() + static_cast<long>(hdrLen + len));
            in_.erase(in_.begin(), in_.begin() + static_cast<long>(hdrLen + len));
            handle(pkt[0], pkt.data() + hdrLen, len);
        }
    }

    // Bytes for the client.
    std::vector<uint8_t> take() {
        std::lock_guard<std::recursive_mutex> g(m_);
        std::vector<uint8_t> v;
        v.swap(out_);
        return v;
    }

    // Another client publishes: sent to ours if a subscription matches,
    // at the lower of the two QoS. false: nothing matched.
    bool deliver(const std::string& topic, const std::vector<uint8_t>& payload, uint8_t qos, bool retain = false) {
        std::lock_guard<std::recursive_mutex> g(m_);
        int best = -1;
        for (auto& s : subs) if (matches(s.first, topic) && static_cast<int>(s.second) > best) best = s.second;
        if (best < 0 || !connected_) return false;
        const uint8_t q = qos < best ? qos : static_cast<uint8_t>(best);
        std::vector<uint8_t> body;
        putStr(body, topic);
        if (q) { ++nextId_; body.push_back(uint8_t(nextId_ >> 8)); body.push_back(uint8_t(nextId_)); }
        body.insert(body.end(), payload.begin(), payload.end());
        send(static_cast<uint8_t>(0x30 | (q << 1) | (retain ? 1 : 0)), body);
        return true;
    }

    // Raw bytes to the client, for malformed-input tests.
    void sendRaw(const std::vector<uint8_t>& b) { std::lock_guard<std::recursive_mutex> g(m_); out_.insert(out_.end(), b.begin(), b.end()); }

    // Runs f(*this) under the broker's lock: for reading the records, or
    // changing the knobs, while the client is talking to it.
    template <typename F>
    auto with(F f) -> decltype(f(*this)) { std::lock_guard<std::recursive_mutex> g(m_); return f(*this); }

    std::vector<Msg> publishedCopy() { std::lock_guard<std::recursive_mutex> g(m_); return published; }
    size_t subCount() { std::lock_guard<std::recursive_mutex> g(m_); return subs.size(); }

    static bool matches(const std::string& filter, const std::string& topic) {
        const std::vector<std::string> f = split(filter), t = split(topic);
        if (!topic.empty() && topic[0] == '$' && !f.empty() && (f[0] == "+" || f[0] == "#")) return false;
        size_t i = 0;
        for (; i < f.size(); ++i) {
            if (f[i] == "#") return true;
            if (i >= t.size()) return false;
            if (f[i] != "+" && f[i] != t[i]) return false;
        }
        return i == t.size();
    }

private:
    static std::vector<std::string> split(const std::string& s) {
        std::vector<std::string> v(1);
        for (char c : s) { if (c == '/') v.emplace_back(); else v.back() += c; }
        return v;
    }
    static void putStr(std::vector<uint8_t>& v, const std::string& s) {
        v.push_back(uint8_t(s.size() >> 8));
        v.push_back(uint8_t(s.size()));
        v.insert(v.end(), s.begin(), s.end());
    }
    static std::string getStr(const uint8_t*& p, const uint8_t* end) {
        if (end - p < 2) return "";
        const size_t n = static_cast<size_t>((p[0] << 8) | p[1]);
        p += 2;
        if (static_cast<size_t>(end - p) < n) { p = end; return ""; }
        std::string s(reinterpret_cast<const char*>(p), n);
        p += n;
        return s;
    }
    void send(uint8_t hdr, const std::vector<uint8_t>& body) {
        if (silent) return;
        out_.push_back(hdr);
        size_t n = body.size();
        do { uint8_t b = n % 128; n /= 128; if (n) b |= 0x80; out_.push_back(b); } while (n);
        out_.insert(out_.end(), body.begin(), body.end());
    }
    bool complete(size_t& len, size_t& hdrLen) {
        if (in_.size() < 2) return false;
        size_t mult = 1, i = 1;
        len = 0;
        for (;; ++i) {
            if (i >= in_.size()) return false;
            len += (in_[i] & 127) * mult;
            mult *= 128;
            if (!(in_[i] & 128)) break;
        }
        hdrLen = i + 1;
        return in_.size() >= hdrLen + len;
    }
    void handle(uint8_t hdr, const uint8_t* b, size_t n) {
        const uint8_t type = hdr >> 4;
        const uint8_t* p = b;
        const uint8_t* end = b + n;
        switch (type) {
        case 1: {   // CONNECT
            ++connects;
            if (getStr(p, end) != "MQTT" || end - p < 4 || p[0] != 4) { protocolError = true; return; }
            connectFlags = p[1];
            keepAlive = static_cast<uint16_t>((p[2] << 8) | p[3]);
            p += 4;
            clientId = getStr(p, end);
            willTopic.clear(); willPayload.clear(); username.clear(); password.clear();
            if (connectFlags & 0x04) { willTopic = getStr(p, end); willPayload = getStr(p, end); }
            if (connectFlags & 0x80) username = getStr(p, end);
            if (connectFlags & 0x40) password = getStr(p, end);
            const bool keep = !(connectFlags & 0x02) && sessionPresent;
            if (!keep) subs.clear();
            send(0x20, {static_cast<uint8_t>(keep && connackCode == 0 ? 1 : 0), connackCode});
            connected_ = connackCode == 0;
            return;
        }
        case 3: {   // PUBLISH
            Msg m;
            m.qos = (hdr >> 1) & 3;
            m.retain = hdr & 1;
            m.dup = (hdr & 8) != 0;
            m.topic = getStr(p, end);
            if (m.qos) { m.id = static_cast<uint16_t>((p[0] << 8) | p[1]); p += 2; }
            m.payload.assign(p, end);
            published.push_back(m);
            if (m.qos == 1 && ackPublishes) send(0x40, {uint8_t(m.id >> 8), uint8_t(m.id)});
            deliver(m.topic, m.payload, m.qos, m.retain);   // to our client too, if it subscribed
            return;
        }
        case 4:     // PUBACK, for a QoS 1 delivery of ours
            pubacks.push_back(static_cast<uint16_t>((b[0] << 8) | b[1]));
            return;
        case 8: {   // SUBSCRIBE
            ++subscribePackets;
            if ((hdr & 0x0F) != 0x02) { protocolError = true; return; }
            const uint16_t id = static_cast<uint16_t>((p[0] << 8) | p[1]);
            p += 2;
            std::vector<uint8_t> body = {uint8_t(id >> 8), uint8_t(id)};
            while (p < end) {
                const std::string f = getStr(p, end);
                const uint8_t q = p < end ? *p++ : 0;
                bool replaced = false;
                for (auto& s : subs) if (s.first == f) { s.second = q; replaced = true; }
                if (!replaced) subs.push_back({f, q});
                body.push_back(q);   // granted as asked
            }
            send(0x90, body);
            return;
        }
        case 10: {  // UNSUBSCRIBE
            if ((hdr & 0x0F) != 0x02) { protocolError = true; return; }
            const uint16_t id = static_cast<uint16_t>((p[0] << 8) | p[1]);
            p += 2;
            while (p < end) {
                const std::string f = getStr(p, end);
                for (size_t i = 0; i < subs.size(); ++i) if (subs[i].first == f) { subs.erase(subs.begin() + static_cast<long>(i)); break; }
            }
            send(0xB0, {uint8_t(id >> 8), uint8_t(id)});
            return;
        }
        case 12:    // PINGREQ
            ++pings;
            if (answerPings) send(0xD0, {});
            return;
        case 14:    // DISCONNECT
            ++disconnects;
            connected_ = false;
            return;
        default:
            protocolError = true;
            return;
        }
    }

    std::recursive_mutex m_;
    std::vector<uint8_t> in_, out_;
    bool connected_ = false;
    uint16_t nextId_ = 0;
};
