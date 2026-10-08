// Host test for xMqttClient: the MQTT thread and the W5500's driver
// thread run for real on std::threads against the FreeRTOS simulation
// in stub/. The simulated chip hands every TCP connection and every
// byte sent on it to SimMqttBroker, and the broker's answers come back
// through the chip; the broker's name is looked up through SimDnsServer.
//
//   g++ -std=c++17 -Wall -Wextra -pthread -Istub -Isim -I../inc -I../hw/freertos/inc
//       -I../w5500/inc -I../mqtt/inc -I<iTransport>/itransport/inc xMqtt_test.cpp
//       ../hw/freertos/src/*.cpp ../w5500/src/*.cpp ../mqtt/src/MqttClient.cpp -o xMqtt_test
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "xEthernet.h"
#include "xMqttClient.h"
#include "W5500.h"
#include "SimW5500.h"
#include "SimDnsServer.h"
#include "SimMqttBroker.h"

using namespace std::chrono;

static int g_failures = 0;
static void check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failures;
}

static long msSince(steady_clock::time_point t0) {
    return static_cast<long>(duration_cast<milliseconds>(steady_clock::now() - t0).count());
}
static void sleepMs(int ms) { std::this_thread::sleep_for(milliseconds(ms)); }

template <typename F>
static bool eventually(F f, int ms = 2000) {
    const auto t0 = steady_clock::now();
    while (msSince(t0) < ms) {
        if (f()) return true;
        sleepMs(2);
    }
    return f();
}

static std::vector<uint8_t> bytes(const char* s) { return std::vector<uint8_t>(s, s + std::strlen(s)); }

typedef W5500::w5500<FakeW5500Spi> Chip;
static const IpAddress kBroker(192, 168, 1, 20);

// A W5500 behind an xEthernet with its driver thread, a broker behind
// the chip, and an xMqttClient with its MQTT thread.
struct Rig {
    SimMqttBroker broker;
    SimDnsServer dnsSrv;
    std::atomic<int> sock{-1};          // the chip socket the broker's connection is on
    std::atomic<int> tcpConnects{0};
    std::unique_ptr<SimW5500> sim{new SimW5500};
    std::unique_ptr<Chip> chip;
    std::unique_ptr<xEthernet> eth;
    std::unique_ptr<xMqttClient> mqtt;
    std::atomic<bool> quit{false};
    std::thread driver, mqttThread;
    bool started = false;

    static xMqttClient::Config defaults() {
        xMqttClient::Config c;
        c.host = "broker.local";
        c.session.clientId = "node-1";
        c.reconnectMinMs = 100;
        c.reconnectMaxMs = 400;
        return c;
    }

    explicit Rig(xMqttClient::Config cfg = defaults(), xMqttClient::Callback cb = nullptr, void* ctx = nullptr) {
        chip.reset(new Chip(W5500::w5500_param_t(), *sim));
        eth.reset(new xEthernet(*chip));
        NetConfig net;
        net.mac = MacAddress(0x02, 0, 0, 0x12, 0x34, 0x56);
        net.ip = IpAddress(192, 168, 1, 50);
        net.subnet = IpAddress(255, 255, 255, 0);
        net.gateway = IpAddress(192, 168, 1, 1);
        net.dhcp = false;
        net.dns = dnsSrv.server;
        dnsSrv.a["broker.local"] = kBroker;
        sim->onUdpSend = [this](uint8_t s, IpAddress dst, uint16_t port, std::vector<uint8_t> data) {
            if (port != 53 || dst != dnsSrv.server) return;
            const std::vector<uint8_t> reply = dnsSrv.handle(data.data(), data.size());
            if (!reply.empty()) sim->peerSendUdp(s, dst, port, reply.data(), reply.size());
        };
        sim->onTcpConnect = [this](uint8_t s, IpAddress dst, uint16_t port) {
            if (dst != kBroker || port != 1883) return;
            broker.newConnection();
            sock = s;
            ++tcpConnects;
        };
        sim->onTcpSend = [this](uint8_t s, std::vector<uint8_t> data) {
            if (s != sock) return;
            broker.feed(data.data(), data.size());
            push();
        };
        eth->begin(net);
        driver = std::thread([this] { while (!quit) eth->service(20); });
        mqtt.reset(new xMqttClient(*eth, cfg));
        if (cb) mqtt->setCallback(cb, ctx);
    }

    bool start() {
        if (!mqtt->begin()) return false;
        mqttThread = std::thread([this] { mqtt->run(); });
        started = true;
        return true;
    }

    // Whatever the broker has for the client, through the chip.
    void push() {
        const std::vector<uint8_t> out = broker.take();
        const int s = sock;
        size_t at = 0;
        for (int tries = 0; at < out.size() && s >= 0 && tries < 1000; ++tries) {
            at += sim->peerSend(static_cast<uint8_t>(s), out.data() + at, out.size() - at);
            if (at < out.size()) sleepMs(1);
        }
    }

    // Another client publishes on the broker.
    bool deliver(const char* topic, const char* payload, uint8_t qos) {
        const bool ok = broker.deliver(topic, bytes(payload), qos);
        push();
        return ok;
    }

    // The broker side drops the TCP connection.
    void drop() { const int s = sock; if (s >= 0) sim->peerClose(static_cast<uint8_t>(s)); }

    template <typename F>
    auto b(F f) -> decltype(f(broker)) { return broker.with(f); }

    void stopMqtt() {
        if (!started) return;
        mqtt->stop();
        mqttThread.join();
        started = false;
    }

    ~Rig() {
        stopMqtt();
        mqtt.reset();
        quit = true;
        driver.join();
    }
};

static int connects(Rig& r) { return r.b([](SimMqttBroker& k) { return k.connects; }); }

int main() {
    std::printf("arguments\n");
    {
        xMqttClient::Config c = Rig::defaults();
        c.host = nullptr;
        Rig r(c);
        check(!r.start(), "begin() refuses a missing broker host");
        c.host = "";
        Rig r2(c);
        check(!r2.start(), "begin() refuses an empty broker host");
        c = Rig::defaults();
        c.maxPacket = 32;
        Rig r3(c);
        check(!r3.start(), "begin() refuses a packet size under 64");
        Rig r4;
        uint8_t small[16];
        MqttMessage m;
        check(!r4.mqtt->publish("t", "x"), "publish() before begin() is refused");
        check(r4.start(), "begin() with a host");
        check(!r4.mqtt->receive(small, sizeof small, m, 0), "receive() refuses a buffer under maxPacket + 4");
    }

    std::printf("connect, subscribe, publish, receive\n");
    {
        xMqttClient::Config c = Rig::defaults();
        c.session.username = "user";
        c.session.password = "pw";
        c.session.keepAliveSec = 30;
        Rig r(c);
        check(r.start(), "begin()");
        const auto t0 = steady_clock::now();
        check(r.mqtt->waitConnected(3000), "connects: looks the broker up, TCP, CONNECT/CONNACK");
        check(msSince(t0) < 1000, "promptly");
        check(r.mqtt->connected(), "connected()");
        check(r.sock >= 0 && r.sim->connectedTo(static_cast<uint8_t>(r.sock.load())) == kBroker &&
                  r.sim->connectedPort(static_cast<uint8_t>(r.sock.load())) == 1883,
              "to the address DNS gave for broker.local, port 1883");
        check(r.b([](SimMqttBroker& k) {
                  return k.clientId == "node-1" && k.username == "user" && k.password == "pw" && k.keepAlive == 30 &&
                         (k.connectFlags & 0x02);
              }),
              "CONNECT carries the client id, credentials, keepalive and clean session");

        check(r.mqtt->subscribe("cmd/#", 1), "subscribe() is granted");
        check(r.broker.subCount() == 1, "the broker has the subscription");

        check(r.mqtt->publish("nodes/node-1/temp", "21.5"), "QoS 0 publish");
        check(eventually([&] { return r.broker.publishedCopy().size() == 1; }), "reaches the broker");
        auto pub = r.broker.publishedCopy();
        check(!pub.empty() && pub[0].topic == "nodes/node-1/temp" && pub[0].payload == bytes("21.5") && pub[0].qos == 0,
              "with its topic and payload");

        const auto t1 = steady_clock::now();
        check(r.mqtt->publish("nodes/node-1/temp", "22.0", 1, true), "QoS 1 publish returns once acknowledged");
        check(msSince(t1) < 1000, "promptly");
        pub = r.broker.publishedCopy();
        check(pub.size() == 2 && pub[1].qos == 1 && pub[1].retain && !pub[1].dup, "QoS 1, retained, not DUP");

        // receive() sleeps until a message comes.
        uint8_t buf[1100];
        MqttMessage m;
        std::thread other([&] { sleepMs(100); r.deliver("cmd/led", "on", 1); });
        const auto t2 = steady_clock::now();
        const bool got = r.mqtt->receive(buf, sizeof buf, m, 3000);
        const long waited = msSince(t2);
        other.join();
        check(got, "receive() gets the message");
        check(waited >= 80 && waited < 1000, "after sleeping until it came");
        check(got && std::strcmp(m.topic, "cmd/led") == 0 && m.len == 2 && std::memcmp(m.payload, "on", 2) == 0 &&
                  m.qos == 1 && !m.retain,
              "topic, payload and QoS");
        check(eventually([&] { return r.b([](SimMqttBroker& k) { return k.pubacks.size() == 1; }); }),
              "the client acknowledged the QoS 1 delivery");
        check(!r.mqtt->receive(buf, sizeof buf, m, 50), "receive() times out with nothing waiting");

        // Our own publishes come back through our own subscription.
        check(r.mqtt->subscribe("nodes/+/temp", 0), "a second subscription");
        check(r.mqtt->publish("nodes/node-1/temp", "23.0", 1), "publish to it");
        check(r.mqtt->receive(buf, sizeof buf, m, 1000) && std::strcmp(m.topic, "nodes/node-1/temp") == 0 &&
                  m.len == 4 && m.qos == 0,
              "comes back at the subscription's QoS 0");

        check(r.mqtt->unsubscribe("nodes/+/temp"), "unsubscribe() is acknowledged");
        check(r.broker.subCount() == 1, "the broker dropped it");
        check(!r.deliver("nodes/x/temp", "1", 0), "nothing is routed to it");

        const xMqttClient::Stats st = r.mqtt->stats();
        check(st.connects == 1 && st.disconnects == 0 && st.published == 3 && st.received == 2 && st.dropped == 0,
              "stats");

        r.stopMqtt();
        check(r.b([](SimMqttBroker& k) { return k.disconnects == 1; }), "stop() says DISCONNECT");
        check(!r.mqtt->connected(), "and is disconnected");
        check(eventually([&] { return r.sim->status(static_cast<uint8_t>(r.sock.load())) == W5500::w5500_SOCK_CLOSED; }),
              "and closed the socket");
    }

    std::printf("reconnect\n");
    {
        Rig r;
        r.start();
        check(r.mqtt->waitConnected(3000), "connected");
        check(r.mqtt->subscribe("cmd/#", 1) && r.mqtt->subscribe("cfg", 0), "two subscriptions");
        check(r.b([](SimMqttBroker& k) { return k.subscribePackets == 2; }), "two SUBSCRIBEs");

        // A QoS 1 message the broker doesn't acknowledge before the drop.
        r.b([](SimMqttBroker& k) { k.ackPublishes = false; return 0; });
        check(!r.mqtt->publish("log", "unacked", 1, false, 200), "publish() times out without a PUBACK");
        r.b([](SimMqttBroker& k) { k.ackPublishes = true; return 0; });

        r.drop();
        check(eventually([&] { return !r.mqtt->connected(); }), "notices the broker closing the connection");
        check(eventually([&] { return connects(r) == 2 && r.mqtt->connected(); }, 3000), "and reconnects");
        check(eventually([&] { return r.broker.subCount() == 2; }), "re-subscribes to both");
        check(r.b([](SimMqttBroker& k) { return k.subscribePackets == 3; }), "in one SUBSCRIBE");
        check(eventually([&] {
                  auto p = r.broker.publishedCopy();
                  return p.size() == 2 && p[1].topic == "log" && p[1].dup && p[1].id == p[0].id;
              }),
              "re-sends the unacknowledged message, DUP, with its id");

        uint8_t buf[1100];
        MqttMessage m;
        r.deliver("cfg", "x", 0);
        check(r.mqtt->receive(buf, sizeof buf, m, 1000) && std::strcmp(m.topic, "cfg") == 0, "receives after the reconnect");
        check(r.mqtt->stats().disconnects == 1 && r.mqtt->stats().connects == 2, "stats count it");
    }

    std::printf("refused, and store and forward\n");
    {
        Rig r;
        r.start();
        check(r.mqtt->waitConnected(3000), "connected");
        r.b([](SimMqttBroker& k) { k.connackCode = 5; return 0; });
        r.drop();
        check(eventually([&] { return r.mqtt->stats().refused >= 2; }, 3000), "refused (not authorised), and retried");
        check(!r.mqtt->connected(), "not connected meanwhile");
        // Backoff: 100, 200, 400, 400 ms ... not a tight loop.
        const int before = connects(r);
        sleepMs(1000);
        const int during = connects(r) - before;
        check(during >= 1 && during <= 4, "retries back off");

        check(!r.mqtt->publish("t", "lost", 0), "QoS 0 publish while disconnected fails");
        check(r.mqtt->publish("t", "kept", 1, false, 0), "QoS 1 publish while disconnected is kept");
        check(!r.mqtt->publish("t", "kept", 1, false, 100), "and doesn't get acknowledged meanwhile");
        check(r.mqtt->subscribe("later", 0), "subscribe() while disconnected is kept");

        r.b([](SimMqttBroker& k) { k.connackCode = 0; return 0; });
        check(r.mqtt->waitConnected(3000), "connects once the broker accepts");
        check(eventually([&] {
                  auto p = r.broker.publishedCopy();
                  int kept = 0;
                  for (auto& m : p) if (m.topic == "t" && m.payload == bytes("kept")) ++kept;
                  return kept == 2;
              }),
              "both kept messages are delivered");
        check(eventually([&] { return r.broker.subCount() == 1; }), "and the subscription is made");
    }

    std::printf("keepalive\n");
    {
        xMqttClient::Config c = Rig::defaults();
        c.session.keepAliveSec = 1;
        Rig r(c);
        r.start();
        check(r.mqtt->waitConnected(3000), "connected");
        check(eventually([&] { return r.b([](SimMqttBroker& k) { return k.pings >= 2; }); }, 3500),
              "PINGREQ every keepalive while idle");
        check(r.mqtt->connected(), "answered pings keep it connected");
        r.b([](SimMqttBroker& k) { k.answerPings = false; return 0; });
        check(eventually([&] { return r.mqtt->stats().disconnects >= 1; }, 6000), "unanswered pings drop the connection");
        r.b([](SimMqttBroker& k) { k.answerPings = true; return 0; });
        check(eventually([&] { return r.mqtt->connected() && connects(r) >= 2; }, 3000), "and it reconnects");
    }

    std::printf("callback\n");
    {
        struct Seen {
            std::mutex m;
            std::vector<std::string> topics;
            xMqttClient* mqtt = nullptr;
            bool refuseNext = false;
        } seen;
        Rig r(Rig::defaults(),
              [](const MqttMessage& m, void* ctx) {
                  Seen& s = *static_cast<Seen*>(ctx);
                  {
                      std::lock_guard<std::mutex> g(s.m);
                      if (s.refuseNext) { s.refuseNext = false; return false; }
                      s.topics.push_back(std::string(m.topic) + "=" +
                                         std::string(reinterpret_cast<const char*>(m.payload), m.len));
                  }
                  // Echo it, QoS 1, from the MQTT thread: mustn't wait for its own ack.
                  if (std::strcmp(m.topic, "cmd/echo") == 0) s.mqtt->publish("echo", m.payload, m.len, 1);
                  return true;
              },
              &seen);
        seen.mqtt = r.mqtt.get();
        r.start();
        check(r.mqtt->waitConnected(3000), "connected");
        check(r.mqtt->subscribe("cmd/#", 1), "subscribed");
        r.deliver("cmd/echo", "ping", 1);
        check(eventually([&] {
                  auto p = r.broker.publishedCopy();
                  return p.size() == 1 && p[0].topic == "echo" && p[0].payload == bytes("ping");
              }),
              "the callback ran and its publish went out");
        {
            std::lock_guard<std::mutex> g(seen.m);
            check(seen.topics.size() == 1 && seen.topics[0] == "cmd/echo=ping", "with the message");
        }
        {
            std::lock_guard<std::mutex> g(seen.m);
            seen.refuseNext = true;
        }
        r.deliver("cmd/x", "1", 1);
        check(eventually([&] { return r.mqtt->stats().dropped == 1; }), "a refused message counts as dropped");
        check(r.b([](SimMqttBroker& k) { return k.pubacks.size() == 1; }), "and isn't acknowledged");
        uint8_t buf[1100];
        MqttMessage m;
        check(!r.mqtt->receive(buf, sizeof buf, m, 50), "nothing goes to receive() in callback mode");
    }

    std::printf("a full inbox\n");
    {
        xMqttClient::Config c = Rig::defaults();
        c.inboxBytes = 64;
        Rig r(c);
        r.start();
        check(r.mqtt->waitConnected(3000), "connected");
        check(r.mqtt->subscribe("s/#", 1), "subscribed");
        for (int i = 0; i < 6; ++i) r.deliver("s/x", "0123456789", 1);
        check(eventually([&] { return r.mqtt->stats().received + r.mqtt->stats().dropped == 6; }), "all six seen");
        const xMqttClient::Stats st = r.mqtt->stats();
        check(st.received >= 1 && st.dropped >= 1, "some fit, the rest are dropped rather than waited for");
        check(r.b([](SimMqttBroker& k) { return k.pubacks.size(); }) == st.received, "only those taken are acknowledged");
        uint8_t buf[1100];
        MqttMessage m;
        uint32_t n = 0;
        while (r.mqtt->receive(buf, sizeof buf, m, 0)) ++n;
        check(n == st.received, "receive() returns those taken");
    }

    std::printf("several publishing threads\n");
    {
        Rig r;
        r.start();
        check(r.mqtt->waitConnected(3000), "connected");
        std::atomic<int> ok{0};
        std::vector<std::thread> ts;
        for (int t = 0; t < 4; ++t) {
            ts.emplace_back([&, t] {
                char topic[16];
                std::snprintf(topic, sizeof topic, "p/%d", t);
                for (int i = 0; i < 25; ++i) {
                    char v[8];
                    std::snprintf(v, sizeof v, "%d", i);
                    if (r.mqtt->publish(topic, v, static_cast<uint8_t>(i & 1), false, 2000)) ++ok;
                }
            });
        }
        for (auto& t : ts) t.join();
        check(ok == 100, "every publish succeeds");
        check(eventually([&] { return r.broker.publishedCopy().size() == 100; }), "all 100 reach the broker");
        bool ordered = true;
        auto p = r.broker.publishedCopy();
        int last[4] = {-1, -1, -1, -1};
        for (auto& m : p) {
            const int t = m.topic[2] - '0';
            const int v = std::atoi(std::string(m.payload.begin(), m.payload.end()).c_str());
            if (v != last[t] + 1) ordered = false;
            last[t] = v;
        }
        check(ordered, "each thread's messages arrive whole and in order");
        check(!r.b([](SimMqttBroker& k) { return k.protocolError; }), "no malformed packets");
    }

    std::printf("%s\n", g_failures ? "FAILED" : "all passed");
    return g_failures ? 1 : 0;
}
