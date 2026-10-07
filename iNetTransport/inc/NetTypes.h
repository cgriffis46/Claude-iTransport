#pragma once
#include <cstdint>

// Plain value types shared by every network interface and chip
// driver. No RTOS, no HAL.

struct IpAddress {
    uint8_t b[4] = {0, 0, 0, 0};

    constexpr IpAddress() = default;
    constexpr IpAddress(uint8_t a, uint8_t b1, uint8_t c, uint8_t d) : b{a, b1, c, d} {}

    constexpr bool isZero() const { return b[0] == 0 && b[1] == 0 && b[2] == 0 && b[3] == 0; }
    constexpr bool operator==(const IpAddress& o) const {
        return b[0] == o.b[0] && b[1] == o.b[1] && b[2] == o.b[2] && b[3] == o.b[3];
    }
    constexpr bool operator!=(const IpAddress& o) const { return !(*this == o); }
};

struct MacAddress {
    uint8_t b[6] = {0, 0, 0, 0, 0, 0};

    constexpr MacAddress() = default;
    constexpr MacAddress(uint8_t a, uint8_t b1, uint8_t c, uint8_t d, uint8_t e, uint8_t f)
        : b{a, b1, c, d, e, f} {}
};

// The interface's own address. With dhcp set, ip/subnet/gateway/dns
// are ignored here and come from a DHCP server instead; the address
// actually in use is reported back through
// iNetDeviceHost::addressChanged() either way. mac is ignored by chips
// that carry their own (most Wi-Fi modules).
struct NetConfig {
    MacAddress mac;
    bool       dhcp = false;
    IpAddress  ip;
    IpAddress  subnet;
    IpAddress  gateway;
    IpAddress  dns;
};
