#pragma once
#include <cstddef>
#include <cstdint>

// An MQTT 3.1.1 client as pure logic: it turns requests into packets
// and packets received into events, and keeps the session's timers and
// state. It sends and receives nothing itself — whoever owns the TCP
// connection feeds it what arrives and sends what it produces:
//
//     mqtt.start(options, now);              // TCP connected: CONNECT queued
//     send mqtt.output(n), mqtt.consume(n)   // whenever output() has bytes
//     mqtt.receive(data, len, now);          // bytes from the broker
//     mqtt.poll(now);                        // keepalive, timeouts
//     state() == Disconnected                // lost: close TCP, reconnect later
//
// QoS 0 and 1 (subscriptions ask for at most 1, so the broker never
// sends QoS 2). What the client keeps across connections:
//   - subscriptions: sent again after every reconnect, unless the
//     broker says it kept the session;
//   - QoS 1 messages not yet acknowledged: sent again, flagged DUP,
//     after every reconnect. A QoS 1 publish while disconnected is kept
//     and sent once connected (store and forward).
//
// Memory is the caller's (Buffers): nothing is allocated here.
class MqttClient {
public:
    static constexpr uint8_t  kMaxInFlight = 4;     // QoS 1 messages awaiting PUBACK
    static constexpr uint8_t  kMaxSubscriptions = 8;
    static constexpr size_t   kMaxFilter = 64;      // characters in a remembered subscription
    static constexpr uint32_t kConnackTimeoutMs = 10000;

    enum class State : uint8_t { Disconnected, Connecting, Connected };

    enum class Error : uint8_t {
        None,
        BadArgument,     // empty or over-long topic, wildcard in a publish topic, QoS > 1, ...
        NotConnected,    // QoS 0 publish or unsubscribe while not connected
        TooBig,          // doesn't fit the output buffer or an in-flight slot
        Busy,            // every in-flight slot or subscription slot is taken, or output is full
        Refused,         // CONNACK refused the connection (refusedCode())
        Protocol,        // the broker sent something malformed or unexpected
        Timeout,         // no CONNACK, or the broker stopped answering pings
    };

    struct Options {
        const char*    clientId = "";       // must be unique on the broker; "" asks it to assign one (clean sessions only)
        const char*    username = nullptr;
        const char*    password = nullptr;
        uint16_t       keepAliveSec = 60;   // 0: no keepalive
        bool           cleanSession = true;
        const char*    willTopic = nullptr; // published by the broker if we vanish
        const uint8_t* willPayload = nullptr;
        size_t         willLen = 0;
        uint8_t        willQos = 0;
        bool           willRetain = false;
    };

    struct Buffers {
        uint8_t* rx;      size_t rxSize;      // largest packet accepted; bigger ones are skipped
        uint8_t* tx;      size_t txSize;      // packets waiting to be sent
        uint8_t* flight;  size_t flightSize;  // QoS 1 messages kept for resending: kMaxInFlight equal slots
    };

    // Told what happens, from inside receive() and poll().
    class Listener {
    public:
        virtual ~Listener() = default;
        virtual void onConnected(bool sessionPresent) = 0;
        // A message from a subscription. Return false if it can't be
        // taken now: a QoS 1 message then isn't acknowledged, and the
        // broker sends it again after the next reconnect.
        virtual bool onMessage(const char* topic, const uint8_t* payload, size_t len, uint8_t qos, bool retain) = 0;
        virtual void onPublished(uint8_t slot) = 0;          // a QoS 1 publish was acknowledged
        virtual void onSubscribed(uint16_t id, bool ok) = 0; // SUBACK (ok: every filter granted)
        virtual void onUnsubscribed(uint16_t id) = 0;
        virtual void onDisconnected(Error why) = 0;          // close the TCP connection
    };

    MqttClient(const Buffers& buffers, Listener& listener);

    // The TCP connection is up: queue CONNECT. Options' strings must
    // stay valid while connected.
    void start(const Options& options, uint32_t nowMs);
    // The TCP connection is gone (whoever owns it noticed). In-flight
    // messages and subscriptions are kept for the next start().
    void connectionLost();
    // Queue DISCONNECT (a clean goodbye: the will isn't published).
    void disconnect();

    void receive(const uint8_t* data, size_t len, uint32_t nowMs);
    void poll(uint32_t nowMs);
    uint32_t nextWakeMs(uint32_t nowMs) const;

    // Bytes to send: output() points at them, consume() drops those sent.
    const uint8_t* output(size_t& len) const { len = txLen_; return tx_; }
    void consume(size_t n);

    // QoS 0: queued now, connected only. QoS 1: kept in a slot (returned
    // in slot) until PUBACK, and sent now if connected. false: error().
    bool publish(const char* topic, const uint8_t* payload, size_t len, uint8_t qos, bool retain,
                 uint32_t nowMs, uint8_t* slot = nullptr);
    // Remembered (and re-sent on every reconnect); sent now if
    // connected, with id. qos 0 or 1.
    bool subscribe(const char* filter, uint8_t qos, uint16_t& id);
    bool unsubscribe(const char* filter, uint16_t& id);

    State    state() const { return state_; }
    Error    error() const { return error_; }
    uint8_t  refusedCode() const { return refused_; }   // CONNACK's return code, 1..5
    uint8_t  inFlight() const;
    uint32_t oversized() const { return oversized_; }    // incoming packets too big for rx, skipped

    // MQTT topic matching: '+' one level, '#' the rest (and the parent).
    static bool topicMatches(const char* filter, const char* topic);

private:
    struct Slot { bool used; bool sent; uint16_t id; uint16_t len; };
    struct Sub  { bool used; uint8_t qos; char filter[kMaxFilter + 1]; };

    bool     queue(const uint8_t* packet, size_t len);
    void     sendPing(uint32_t nowMs);
    void     resendInFlight();
    void     resubscribe();
    void     dispatch(uint8_t hdr, uint8_t* body, size_t len, uint32_t nowMs);
    void     drop(Error why);
    uint16_t nextId();
    uint8_t* slotData(uint8_t i) { return flight_ + i * slotSize_; }

    uint8_t*  rx_;
    size_t    rxSize_;
    uint8_t*  tx_;
    size_t    txSize_;
    size_t    txLen_ = 0;
    uint8_t*  flight_;
    size_t    slotSize_;
    Listener& listener_;

    State     state_ = State::Disconnected;
    Error     error_ = Error::None;
    uint8_t   refused_ = 0;
    Options   opt_;
    uint16_t  lastId_ = 0;
    Slot      slots_[kMaxInFlight] = {};
    bool      everSent_[kMaxInFlight] = {};  // gone out at least once: DUP when sent again
    Sub       subs_[kMaxSubscriptions] = {};

    uint32_t  startedAt_ = 0, lastTx_ = 0, lastRx_ = 0;
    bool      pingOut_ = false;
    uint32_t  pingAt_ = 0;
    bool      resendPending_ = false;   // in-flight messages still to resend after CONNACK
    bool      resubPending_ = false;

    // Incoming stream: fixed header, remaining length, body.
    uint8_t   hdr_ = 0;
    bool      haveHdr_ = false;
    uint8_t   lenBytes_ = 0;
    bool      inBody_ = false;
    uint32_t  remaining_ = 0, mult_ = 1, got_ = 0;
    bool      skipping_ = false;
    uint32_t  oversized_ = 0;
};
