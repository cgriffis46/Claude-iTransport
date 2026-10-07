#pragma once
#include "xNetInterface.h"

// A wireless interface: any iWifiDevice — an ESP-AT module over UART
// (iTransport), an ATWINC1500 over SPI (iBlockTransport), ... — with
// the same sockets and xClient as xEthernet, plus joining a network.
//
//     xWifi wifi(module);
//     wifi.begin(netConfig);                       // then start the driver thread
//     if (wifi.join("my-ssid", "passphrase", 15000)) {
//         xClient client(wifi);
//         ...
//     }
//
// linkUp()/waitLinkUp() report association.
class xWifi : public xNetInterface {
public:
    explicit xWifi(iWifiDevice& dev, const Config& cfg = Config())
        : xNetInterface(dev, cfg), wifi_(dev) {}

    static constexpr size_t kMaxSsid = 32;
    static constexpr size_t kMaxPassphrase = 64;

    // Joins, sleeping until associated, refused, or the timeout. The
    // strings are copied. false also for an over-long SSID/passphrase.
    // Call from one thread at a time.
    bool join(const char* ssid, const char* passphrase, uint32_t timeoutMs);

    // Leaves the network, waiting up to timeoutMs for the link to drop.
    // Every connection is lost.
    void leave(uint32_t timeoutMs = 1000);

protected:
    void handleOther(const Msg& m) override;

private:
    iWifiDevice& wifi_;
    char ssid_[kMaxSsid + 1] = {0};
    char pass_[kMaxPassphrase + 1] = {0};
};
