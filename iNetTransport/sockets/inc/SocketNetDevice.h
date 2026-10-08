#pragma once
#include <cstddef>
#include <cstdint>
#include "iNetDevice.h"
#include "DnsClient.h"
#include "SntpClient.h"

// An iNetDevice on a BSD socket API: lwIP's (build with
// INET_SOCKETS_LWIP), for an MCU with its own Ethernet MAC such as the
// STM32F207, or an ESP32 under ESP-IDF; or the operating system's, on
// Linux. With it, xEthernet, xClient and everything built on them
// (xHttpServer, xHttpClient, xMqttClient) run unchanged over a host
// TCP/IP stack instead of a socket-offload chip:
//
//     static SocketNetDevice::Config devCfg;          // lwIP: devCfg.platform = lwipNetifPlatform();
//     static SocketNetDevice dev(devCfg);
//     static xEthernet eth(dev);
//     eth.begin(net);                                 // then eth.run() on its thread, as with the W5500
//
// The stack owns the interface: the MAC, the PHY, and (on lwIP) the
// address, DHCP included, are set up by the application (CubeMX's
// MX_LWIP_Init(), say). The device only reads them, through Platform.
// NetConfig::dhcp is not used; NetConfig::dns is the DNS server unless
// Platform::address gives one.
//
// Sockets: socketCount() interface sockets, each a TCP client, or a
// listener's connection, at a time. Several sockets listening on one
// port share one listening socket, so connections wait in its backlog
// rather than being refused between accepts. DNS and SNTP use one UDP
// socket of the device's own, with DnsClient and SntpClient.
//
// Non-blocking like every iNetDevice: poll() looks at every socket with
// a zero-timeout select() and moves what's ready, and asks to be called
// again within Config::activePollMs while anything is open. On lwIP
// that's one lwip_select() per poll.
class SocketNetDevice : public iEthernetDevice {
public:
    static constexpr uint8_t kMaxSockets = 8;
    static constexpr uint8_t kMaxListeners = 4;
    static constexpr size_t  kRecvChunk = 512;   // bytes per recv(), shared
    static constexpr size_t  kSendChunk = 256;   // bytes per send(), per socket

    // How to see the interface the stack owns. The defaults suit an
    // operating system: the link is up and the address is the one
    // configure() was given.
    struct Platform {
        bool (*linkUp)(void* ctx) = nullptr;
        // Fills ip, subnet, gateway and, if it knows one, dns. false: no
        // address yet (DHCP still going).
        bool (*address)(NetConfig& out, void* ctx) = nullptr;
        void* ctx = nullptr;
    };

    struct Config {
        uint8_t  sockets = kMaxSockets;   // offered to the interface, up to kMaxSockets
        uint32_t activePollMs = 2;        // poll() interval while a socket is open
        uint32_t idlePollMs = 50;         // and while none is
        int      listenBacklog = 4;
        uint32_t listenLingerMs = 2000;   // a listening socket nobody uses is closed after this
        Platform platform;
    };

    SocketNetDevice() : SocketNetDevice(Config()) {}
    explicit SocketNetDevice(const Config& cfg);
    ~SocketNetDevice() override;

    SocketNetDevice(const SocketNetDevice&) = delete;
    SocketNetDevice& operator=(const SocketNetDevice&) = delete;

    // iNetDevice
    void     attach(iNetDeviceHost& host) override { host_ = &host; }
    uint8_t  socketCount() const override { return cfg_.sockets; }
    void     configure(const NetConfig& cfg) override;
    uint32_t poll(uint32_t nowMs) override;
    bool     connect(uint8_t s, const IpAddress& ip, uint16_t port, uint16_t localPort) override;
    bool     listen(uint8_t s, uint16_t port) override;
    void     close(uint8_t s) override;
    void     interrupt() override {}
    bool     resolve(const char* name) override;
    bool     requestTime(const char* server) override;

    // iEthernetDevice: the stack doesn't say, so 100 Mbit/s full duplex
    // while the link is up.
    uint16_t speedMbps() const override { return link_ ? 100 : 0; }
    bool     fullDuplex() const override { return link_; }

private:
    enum class St : uint8_t { Free, Connecting, Listening, Open };
    struct Sock {
        int      fd = -1;
        St       st = St::Free;
        uint8_t  listener = 0xFF;          // while Listening
        uint8_t  pend[kSendChunk];         // taken from the host, not yet sent
        uint16_t pendLen = 0, pendAt = 0;
    };
    struct Listener {
        int      fd = -1;
        uint16_t port = 0;
        uint8_t  users = 0;                // sockets listening through it
        uint32_t idleSince = 0;
    };
    struct Queued { uint8_t s; SocketEvent ev; };

    void queue(uint8_t s, SocketEvent ev);
    void purge(uint8_t s);
    void drop(uint8_t s);                  // forget socket s's connection, without an event
    void lost(uint8_t s, SocketEvent ev);  // drop it and say so
    void dropAll();                        // the address or link went
    void checkInterface(uint32_t nowMs);
    void serviceQueries(uint32_t nowMs);
    void udpSend(size_t len, const IpAddress& dst, uint16_t port);
    void failQueries(uint32_t nowMs);
    uint32_t nextRand();

    Config          cfg_;
    iNetDeviceHost* host_ = nullptr;
    NetConfig       net_;                  // from configure()
    NetConfig       have_;                 // what the host was last told
    bool            configured_ = false, readySent_ = false, link_ = false, addr_ = false;

    Sock            socks_[kMaxSockets];
    Listener        listeners_[kMaxListeners];
    Queued          events_[2 * kMaxSockets];
    uint8_t         eventCount_ = 0;
    uint8_t         buf_[kRecvChunk];      // recv(), and DNS/SNTP packets

    int             udp_ = -1;
    DnsClient       dns_, ntpDns_;
    SntpClient      sntp_;
    bool            dnsReq_ = false, ntpReq_ = false, ntpBusy_ = false;
    char            dnsName_[DnsClient::kMaxName + 2] = {0};
    char            ntpServer_[DnsClient::kMaxName + 2] = {0};
    uint32_t        rand_ = 0x9E3779B9u;
};
