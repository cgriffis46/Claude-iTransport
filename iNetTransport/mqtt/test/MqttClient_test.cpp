// Host test for MqttClient against the simulated broker in
// test/sim/SimMqttBroker.h, on a simulated clock.
//
//   g++ -std=c++14 -Wall -Wextra -I../inc -I../../test/sim
//       MqttClient_test.cpp ../src/MqttClient.cpp -o MqttClient_test
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "MqttClient.h"
#include "SimMqttBroker.h"

static int g_failures = 0;
static void check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failures;
}

static std::vector<uint8_t> bytes(const char* s) { return std::vector<uint8_t>(s, s + std::strlen(s)); }

struct Rig : MqttClient::Listener {
    uint8_t rx[512], tx[512], flight[4 * 256];
    MqttClient mqtt{MqttClient::Buffers{rx, sizeof rx, tx, sizeof tx, flight, sizeof flight}, *this};
    SimMqttBroker broker;
    MqttClient::Options opt;
    uint32_t now = 1000;
    bool link = true;           // the TCP connection carries bytes
    bool chunked = false;       // hand bytes to the client one at a time

    // What the listener heard.
    int connected = 0, disconnected = 0, subscribed = 0, unsubscribed = 0;
    bool sessionPresent = false, subOk = false;
    MqttClient::Error why = MqttClient::Error::None;
    std::vector<uint8_t> published;   // slots
    struct In { std::string topic; std::string payload; uint8_t qos; bool retain; };
    std::vector<In> in;
    bool refuse = false;              // onMessage says no

    void onConnected(bool sp) override { ++connected; sessionPresent = sp; }
    bool onMessage(const char* t, const uint8_t* p, size_t n, uint8_t q, bool r) override {
        if (refuse) return false;
        in.push_back({t, std::string(reinterpret_cast<const char*>(p), n), q, r});
        return true;
    }
    void onPublished(uint8_t slot) override { published.push_back(slot); }
    void onSubscribed(uint16_t, bool ok) override { ++subscribed; subOk = ok; }
    void onUnsubscribed(uint16_t) override { ++unsubscribed; }
    void onDisconnected(MqttClient::Error e) override { ++disconnected; why = e; }

    Rig() { opt.clientId = "node-1"; }

    // Moves bytes both ways until nothing moves.
    void pump() {
        for (int i = 0; i < 50 && link; ++i) {
            size_t n = 0;
            const uint8_t* out = mqtt.output(n);
            if (n) { broker.feed(out, n); mqtt.consume(n); }
            const std::vector<uint8_t> back = broker.take();
            if (chunked) for (uint8_t b : back) mqtt.receive(&b, 1, now);
            else if (!back.empty()) mqtt.receive(back.data(), back.size(), now);
            if (n == 0 && back.empty()) break;
        }
    }
    void connect() { broker.newConnection(); mqtt.start(opt, now); pump(); }
    void run(uint32_t ms) {
        for (uint32_t t = 0; t < ms; t += 10, now += 10) { mqtt.poll(now); pump(); }
    }
};

int main() {
    std::printf("CONNECT\n");
    {
        Rig r;
        r.opt.username = "user";
        r.opt.password = "pass";
        r.opt.keepAliveSec = 30;
        r.opt.willTopic = "nodes/node-1/status";
        r.opt.willPayload = reinterpret_cast<const uint8_t*>("gone");
        r.opt.willLen = 4;
        r.opt.willQos = 1;
        r.opt.willRetain = true;
        r.mqtt.start(r.opt, r.now);
        size_t n = 0;
        const uint8_t* out = r.mqtt.output(n);
        const uint8_t head[] = {0x10};
        check(n > 10 && out[0] == head[0], "fixed header CONNECT");
        const uint8_t varHead[] = {0, 4, 'M', 'Q', 'T', 'T', 4, 0xEE, 0, 30};
        check(std::memcmp(out + 2, varHead, sizeof varHead) == 0,
              "protocol MQTT level 4; flags user, password, will retain, will QoS 1, will, clean; keepalive 30");
        r.pump();
        check(r.broker.clientId == "node-1" && r.broker.username == "user" && r.broker.password == "pass" &&
              r.broker.willTopic == "nodes/node-1/status" && r.broker.willPayload == "gone", "payload fields as the broker reads them");
        check(r.connected == 1 && r.mqtt.state() == MqttClient::State::Connected && !r.sessionPresent, "CONNACK: Connected");
    }

    std::printf("refused, and no answer\n");
    {
        Rig r;
        r.broker.connackCode = 5;
        r.connect();
        check(r.mqtt.state() == MqttClient::State::Disconnected && r.why == MqttClient::Error::Refused && r.mqtt.refusedCode() == 5,
              "CONNACK 5 (not authorised): Disconnected, Refused, code kept");
        Rig s;
        s.broker.silent = true;
        s.connect();
        s.run(9990);
        check(s.mqtt.state() == MqttClient::State::Connecting, "waiting for CONNACK...");
        s.run(20);
        check(s.mqtt.state() == MqttClient::State::Disconnected && s.why == MqttClient::Error::Timeout, "...10 s, then Timeout");
    }

    std::printf("publish\n");
    {
        Rig r;
        r.connect();
        check(r.mqtt.publish("a/b", reinterpret_cast<const uint8_t*>("hi"), 2, 0, true, r.now), "QoS 0 publish");
        r.pump();
        auto pub = r.broker.publishedCopy();
        check(pub.size() == 1 && pub[0].topic == "a/b" && pub[0].payload == bytes("hi") && pub[0].qos == 0 && pub[0].retain,
              "arrives: topic, payload, retain");
        uint8_t slot = 99;
        check(r.mqtt.publish("a/c", reinterpret_cast<const uint8_t*>("x"), 1, 1, false, r.now, &slot) && slot == 0, "QoS 1 publish, slot 0");
        check(r.mqtt.inFlight() == 1, "in flight until acknowledged");
        r.pump();
        check(r.published.size() == 1 && r.published[0] == 0 && r.mqtt.inFlight() == 0, "PUBACK: onPublished(0), slot free");
        check(r.broker.publishedCopy()[1].id != 0 && !r.broker.publishedCopy()[1].dup, "with a packet id, not DUP");

        r.broker.ackPublishes = false;
        for (int i = 0; i < 4; ++i) r.mqtt.publish("q", reinterpret_cast<const uint8_t*>("x"), 1, 1, false, r.now);
        check(!r.mqtt.publish("q", reinterpret_cast<const uint8_t*>("x"), 1, 1, false, r.now) &&
              r.mqtt.error() == MqttClient::Error::Busy, "five unacknowledged QoS 1: the fifth is refused, Busy");

        check(!r.mqtt.publish("a/+", nullptr, 0, 0, false, r.now) && r.mqtt.error() == MqttClient::Error::BadArgument, "wildcard topic refused");
        check(!r.mqtt.publish("", nullptr, 0, 0, false, r.now), "empty topic refused");
        check(!r.mqtt.publish("t", nullptr, 0, 2, false, r.now), "QoS 2 refused");
        std::vector<uint8_t> big(600, 'z');
        check(!r.mqtt.publish("t", big.data(), big.size(), 0, false, r.now) && r.mqtt.error() == MqttClient::Error::TooBig,
              "bigger than the output buffer: TooBig");
    }

    std::printf("store and forward, DUP on resend\n");
    {
        Rig r;
        check(!r.mqtt.publish("t", reinterpret_cast<const uint8_t*>("x"), 1, 0, false, r.now) &&
              r.mqtt.error() == MqttClient::Error::NotConnected, "QoS 0 while disconnected: NotConnected");
        check(r.mqtt.publish("early", reinterpret_cast<const uint8_t*>("1"), 1, 1, false, r.now), "QoS 1 while disconnected: kept");
        r.connect();
        auto pub = r.broker.publishedCopy();
        check(pub.size() == 1 && pub[0].topic == "early" && !pub[0].dup && r.published.size() == 1, "sent after CONNACK, acknowledged");

        r.broker.ackPublishes = false;
        r.mqtt.publish("lost", reinterpret_cast<const uint8_t*>("2"), 1, 1, false, r.now);
        r.pump();
        r.mqtt.connectionLost();           // the TCP connection dropped
        r.broker.ackPublishes = true;
        r.connect();
        pub = r.broker.publishedCopy();
        check(pub.size() == 3 && pub[2].topic == "lost" && pub[2].dup && pub[2].id == pub[1].id,
              "unacknowledged message sent again after reconnecting: same id, DUP");
        check(r.mqtt.inFlight() == 0, "and acknowledged this time");
    }

    std::printf("subscribe and receive\n");
    {
        Rig r;
        r.connect();
        uint16_t id = 0;
        check(r.mqtt.subscribe("sensors/+/temp", 1, id) && id != 0, "subscribe");
        r.pump();
        check(r.subscribed == 1 && r.subOk && r.broker.subCount() == 1, "SUBACK");
        r.broker.deliver("sensors/kitchen/temp", bytes("21.5"), 1);
        r.pump();
        check(r.in.size() == 1 && r.in[0].topic == "sensors/kitchen/temp" && r.in[0].payload == "21.5" && r.in[0].qos == 1,
              "a QoS 1 message arrives");
        check(r.broker.pubacks.size() == 1, "and is acknowledged");
        r.chunked = true;
        r.broker.deliver("sensors/hall/temp", bytes("19.0"), 0, true);
        r.pump();
        check(r.in.size() == 2 && r.in[1].payload == "19.0" && r.in[1].retain && r.in[1].qos == 0,
              "byte by byte: QoS 0, retained");
        r.chunked = false;
        r.refuse = true;
        r.broker.deliver("sensors/attic/temp", bytes("30"), 1);
        r.pump();
        check(r.broker.pubacks.size() == 1, "refused by the listener: not acknowledged (the broker sends it again later)");
        r.refuse = false;
        check(!r.mqtt.subscribe("a/#/b", 0, id) && !r.mqtt.subscribe("a+", 0, id), "bad filters refused");
        check(r.mqtt.unsubscribe("sensors/+/temp", id), "unsubscribe");
        r.pump();
        check(r.unsubscribed == 1 && r.broker.subCount() == 0, "UNSUBACK, gone at the broker");
    }

    std::printf("subscriptions across reconnects\n");
    {
        Rig r;
        r.connect();
        uint16_t id;
        r.mqtt.subscribe("a/#", 0, id);
        r.mqtt.subscribe("b", 1, id);
        r.pump();
        r.mqtt.connectionLost();
        r.connect();
        check(r.broker.subCount() == 2 && r.broker.subscribePackets == 3, "clean session: both sent again, in one SUBSCRIBE");

        Rig p;
        p.opt.cleanSession = false;
        p.broker.sessionPresent = true;
        p.connect();
        p.mqtt.subscribe("x", 1, id);
        p.pump();
        p.mqtt.connectionLost();
        p.connect();
        check(p.sessionPresent && p.broker.subscribePackets == 1, "the broker kept the session: not sent again");

        Rig q;
        q.mqtt.subscribe("pre/connect", 1, id);
        q.connect();
        check(q.broker.subCount() == 1, "subscribed before connecting: sent on CONNACK");
    }

    std::printf("keepalive\n");
    {
        Rig r;
        r.opt.keepAliveSec = 10;
        r.connect();
        r.run(9900);
        check(r.broker.pings == 0, "quiet for less than the keepalive: no ping");
        r.run(200);
        check(r.broker.pings == 1 && r.mqtt.state() == MqttClient::State::Connected, "PINGREQ at 10 s, answered");
        r.broker.answerPings = false;
        r.run(10000);
        check(r.broker.pings == 2 && r.mqtt.state() == MqttClient::State::Connected, "another ping, unanswered...");
        r.run(5100);
        check(r.mqtt.state() == MqttClient::State::Disconnected && r.why == MqttClient::Error::Timeout,
              "...gone after half the keepalive: Timeout");
        check(r.mqtt.nextWakeMs(r.now) == 0xFFFFFFFFu, "nothing to wake for while disconnected");
    }

    std::printf("malformed and oversized input\n");
    {
        Rig r;
        r.connect();
        uint16_t id;
        r.mqtt.subscribe("#", 0, id);
        r.pump();
        std::vector<uint8_t> big(2000, 'x');
        r.broker.deliver("big", big, 0);
        r.broker.deliver("small", bytes("ok"), 0);
        r.pump();
        check(r.mqtt.oversized() == 1 && r.in.size() == 1 && r.in[0].topic == "small",
              "a packet bigger than rx is skipped; the next one still arrives");
        r.broker.sendRaw({0x30, 0xFF, 0xFF, 0xFF, 0xFF, 0x01});
        r.pump();
        check(r.mqtt.state() == MqttClient::State::Disconnected && r.why == MqttClient::Error::Protocol,
              "a five-byte remaining length: Protocol");
        Rig s;
        s.connect();
        s.broker.sendRaw({0x34, 0x05, 0x00, 0x01, 't', 0x00, 0x01});   // QoS 2
        s.pump();
        check(s.why == MqttClient::Error::Protocol, "a QoS 2 publish we never asked for: Protocol");
        Rig t;
        t.connect();
        t.broker.sendRaw({0x30, 0x03, 0x00, 0x05, 'x'});   // topic length past the end
        t.pump();
        check(t.why == MqttClient::Error::Protocol, "a topic running off the end: Protocol");
    }

    std::printf("topic matching\n");
    {
        struct Case { const char* f; const char* t; bool m; } cases[] = {
            {"a/b", "a/b", true}, {"a/b", "a/c", false}, {"a/+", "a/b", true}, {"a/+", "a/b/c", false},
            {"a/#", "a/b/c", true}, {"a/#", "a", true}, {"#", "a/b", true}, {"+/+", "a/b", true},
            {"+", "/a", false}, {"/+", "/a", true}, {"sport/+", "sport/", true}, {"+/b", "a/b", true},
            {"#", "$SYS/x", false}, {"$SYS/#", "$SYS/x", true}, {"a/bc", "a/b", false}, {"a/b", "a/bc", false},
        };
        bool all = true;
        for (const Case& c : cases) {
            const bool m = MqttClient::topicMatches(c.f, c.t);
            if (m != c.m) { std::printf("    %s vs %s: got %d\n", c.f, c.t, m); all = false; }
            if (SimMqttBroker::matches(c.f, c.t) != c.m) { std::printf("    broker disagrees: %s vs %s\n", c.f, c.t); all = false; }
        }
        check(all, "16 cases: + and # levels, a/# matching a, $ topics");
    }

    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "PASSED", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
