#include "LwipNetif.h"
#include <cstring>
#include "lwip/netif.h"
#include "lwip/tcpip.h"
#include "lwip/dns.h"
#include "lwip/ip4_addr.h"

namespace {

struct netif* pick(void* ctx) {
    return ctx ? static_cast<struct netif*>(ctx) : netif_default;
}

void lockCore() {
#if LWIP_TCPIP_CORE_LOCKING
    LOCK_TCPIP_CORE();
#endif
}

void unlockCore() {
#if LWIP_TCPIP_CORE_LOCKING
    UNLOCK_TCPIP_CORE();
#endif
}

IpAddress fromLwip(const ip4_addr_t* a) {
    IpAddress ip;
    std::memcpy(ip.b, &a->addr, 4);   // network order, as IpAddress keeps it
    return ip;
}

bool linkUp(void* ctx) {
    lockCore();
    struct netif* n = pick(ctx);
    const bool up = n != nullptr && netif_is_up(n) && netif_is_link_up(n);
    unlockCore();
    return up;
}

bool address(NetConfig& out, void* ctx) {
    lockCore();
    struct netif* n = pick(ctx);
    bool ok = false;
    if (n != nullptr && !ip4_addr_isany_val(*netif_ip4_addr(n))) {
        out.ip = fromLwip(netif_ip4_addr(n));
        out.subnet = fromLwip(netif_ip4_netmask(n));
        out.gateway = fromLwip(netif_ip4_gw(n));
#if LWIP_DNS
        const ip_addr_t* d = dns_getserver(0);
        if (d != nullptr && IP_IS_V4(d) && !ip4_addr_isany_val(*ip_2_ip4(d))) out.dns = fromLwip(ip_2_ip4(d));
#endif
        ok = true;
    }
    unlockCore();
    return ok;
}

}  // namespace

SocketNetDevice::Platform lwipNetifPlatform(struct netif* nif) {
    SocketNetDevice::Platform p;
    p.linkUp = linkUp;
    p.address = address;
    p.ctx = nif;
    return p;
}
