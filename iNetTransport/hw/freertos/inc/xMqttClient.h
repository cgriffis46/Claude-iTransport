#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include "FreeRTOS.h"
#include "semphr.h"
#include "event_groups.h"
#include "message_buffer.h"
#include "task.h"
#include "MqttClient.h"
#include "xClient.h"
#include "xNetInterface.h"

// A message received from a subscription. topic is NUL-terminated;
// both point into the buffer given to receive().
struct MqttMessage {
    const char*    topic = nullptr;
    const uint8_t* payload = nullptr;
    size_t         len = 0;
    uint8_t        qos = 0;
    bool           retain = false;
};

// An MQTT 3.1.1 client on any xNetInterface (xEthernet, xWifi), for
// FreeRTOS. Its own thread (run()) keeps the session: it looks up the
// broker, connects, reconnects with backoff when the connection drops,
// keeps the session alive, and re-subscribes and re-sends unacknowledged
// QoS 1 messages after a reconnect. Other threads use it with blocking
// calls that sleep up to a timeout:
//
//     xMqttClient::Config cfg;
//     cfg.host = "broker.local";               // a name or "a.b.c.d"
//     cfg.session.clientId = "node-1";
//     static xMqttClient mqtt(eth, cfg);
//     mqtt.begin();
//     osThreadNew([](void* m) { static_cast<xMqttClient*>(m)->run(); }, &mqtt, &attrs);
//
//     mqtt.subscribe("nodes/node-1/cmd", 1);
//     mqtt.publish("nodes/node-1/temp", "21.5");
//     uint8_t buf[1100];
//     MqttMessage m;
//     if (mqtt.receive(buf, sizeof buf, m, 5000)) ...   // sleeps until a message
//
// Writes go straight out from the calling thread, under the session's
// mutex, so a publish isn't held up behind the MQTT thread. Received
// messages wait in a FreeRTOS message buffer for receive() — or, with
// setCallback(), go to a function on the MQTT thread instead.
//
// The protocol is MqttClient's (mqtt/): QoS 0 and 1, no TLS.
class xMqttClient : private MqttClient::Listener {
public:
    struct Config {
        const char* host = nullptr;          // the broker: a name or "a.b.c.d"; must outlive the client
        uint16_t    port = 1883;
        MqttClient::Options session;         // client id, credentials, keepalive, will, clean session
        size_t      maxPacket = 1024;        // largest packet either way; from the FreeRTOS heap, twice
        size_t      inFlightBytes = 2048;    // unacknowledged QoS 1 messages, MqttClient::kMaxInFlight slots
        size_t      inboxBytes = 2048;       // received messages waiting for receive()
        uint32_t    connectTimeoutMs = 10000;
        uint32_t    reconnectMinMs = 1000;   // backoff after a failed or lost connection,
        uint32_t    reconnectMaxMs = 30000;  // doubling up to this
    };

    struct Stats {
        uint32_t connects, disconnects, refused, published, received, dropped;
    };

    // Called on the MQTT thread for each message, instead of queueing it
    // for receive(). Return false to refuse it (a QoS 1 message is then
    // sent again by the broker after the next reconnect). It may
    // publish, but a QoS 1 publish from here doesn't wait for its ack.
    typedef bool (*Callback)(const MqttMessage& m, void* ctx);

    xMqttClient(xNetInterface& net, const Config& cfg);
    ~xMqttClient() override;

    xMqttClient(const xMqttClient&) = delete;
    xMqttClient& operator=(const xMqttClient&) = delete;

    void setCallback(Callback cb, void* ctx) { cb_ = cb; cbCtx_ = ctx; }   // before begin()

    // Allocates the buffers and FreeRTOS objects. Call once, before
    // run() and anything else. false: out of FreeRTOS heap.
    bool begin();

    // The MQTT thread's body. Returns only after stop().
    void run();

    // Ends the session with a DISCONNECT (so the will isn't published),
    // and makes run() return within about a second.
    void stop();

    bool connected() const;
    bool waitConnected(uint32_t timeoutMs);

    // QoS 0: true once queued on the connection (false while not
    // connected). QoS 1: kept until the broker acknowledges it, even
    // across reconnects, and sent once connected; true when acknowledged
    // within timeoutMs (timeoutMs 0: once kept). false also when every
    // QoS 1 slot is waiting (MqttClient::kMaxInFlight) or the message is
    // bigger than Config::maxPacket allows.
    bool publish(const char* topic, const void* payload, size_t len, uint8_t qos = 0, bool retain = false,
                 uint32_t timeoutMs = 2000);
    bool publish(const char* topic, const char* text, uint8_t qos = 0, bool retain = false, uint32_t timeoutMs = 2000);

    // Subscribed, and re-subscribed after every reconnect. Connected:
    // true when the broker grants it within timeoutMs. Not connected:
    // true at once, and sent when the connection is up.
    bool subscribe(const char* filter, uint8_t qos = 0, uint32_t timeoutMs = 5000);
    bool unsubscribe(const char* filter, uint32_t timeoutMs = 5000);

    // The next received message, sleeping up to timeoutMs for one. buf
    // holds it: at least Config::maxPacket + 4 bytes (smaller is refused).
    bool receive(uint8_t* buf, size_t cap, MqttMessage& out, uint32_t timeoutMs);

    Stats stats() const;

private:
    // MqttClient::Listener, on whichever thread holds the session.
    void onConnected(bool sessionPresent) override;
    bool onMessage(const char* topic, const uint8_t* payload, size_t len, uint8_t qos, bool retain) override;
    void onPublished(uint8_t slot) override;
    void onSubscribed(uint16_t id, bool ok) override;
    void onUnsubscribed(uint16_t id) override;
    void onDisconnected(MqttClient::Error why) override;

    bool lock(uint32_t timeoutMs);
    void unlock();
    void flushLocked();      // MqttClient's output to the socket
    void closeLocked();      // the TCP connection is over; the MQTT thread only
    void backoff();
    bool waitAck(uint8_t slot, uint32_t gen, uint32_t timeoutMs);
    static uint32_t nowMs();

    xNetInterface& net_;
    Config         cfg_;
    xClient        client_;
    MqttClient*    core_ = nullptr;
    alignas(MqttClient) unsigned char coreMem_[sizeof(MqttClient)];

    uint8_t*          rx_ = nullptr;
    uint8_t*          tx_ = nullptr;
    uint8_t*          flight_ = nullptr;
    uint8_t*          scratch_ = nullptr;   // one message, framed for the inbox
    uint8_t           readBuf_[256];        // the MQTT thread's reads
    MessageBufferHandle_t inbox_ = nullptr;
    SemaphoreHandle_t session_ = nullptr;   // recursive: a callback may publish
    SemaphoreHandle_t requestMutex_ = nullptr;   // one subscribe/unsubscribe at a time
    SemaphoreHandle_t requestDone_ = nullptr;
    SemaphoreHandle_t recvMutex_ = nullptr;
    EventGroupHandle_t events_ = nullptr;
    // QoS 1 acks, per in-flight slot. A slot is reused as soon as its
    // message is acknowledged, maybe before that message's publisher has
    // woken, so each publish into a slot is numbered (sentGen_) and each
    // ack records the number it acknowledged (ackedGen_); both under
    // session_. An event bit per slot (kAck << slot) wakes the waiters.
    uint32_t          sentGen_[MqttClient::kMaxInFlight] = {};
    uint32_t          ackedGen_[MqttClient::kMaxInFlight] = {};
    std::atomic<TaskHandle_t> thread_{nullptr};

    static constexpr EventBits_t kConnected = 1u << 0;
    static constexpr EventBits_t kStop = 1u << 1;
    static constexpr EventBits_t kAck = 1u << 2;   // .. 1u << (2 + kMaxInFlight - 1)

    Callback          cb_ = nullptr;
    void*             cbCtx_ = nullptr;
    bool              tcpUp_ = false;       // under session_; only the MQTT thread changes it
    uint16_t          pendingId_ = 0;       // the subscribe/unsubscribe a caller waits for
    bool              pendingOk_ = false;
    uint32_t          backoffMs_ = 0;
    std::atomic<bool> stopping_{false};

    std::atomic<uint32_t> stConnects_{0}, stDisconnects_{0}, stRefused_{0}, stPublished_{0}, stReceived_{0},
                          stDropped_{0};
};
