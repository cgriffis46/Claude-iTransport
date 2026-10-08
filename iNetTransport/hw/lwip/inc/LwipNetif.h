#pragma once
#include "SocketNetDevice.h"

struct netif;

// SocketNetDevice's view of an lwIP netif: up and linked, and its
// address, mask, gateway and first DNS server, as lwIP has them (DHCP
// included). nif: nullptr for lwIP's default netif, looked up each time.
//
//     SocketNetDevice::Config devCfg;
//     devCfg.platform = lwipNetifPlatform(&gnetif);   // CubeMX's
//     static SocketNetDevice dev(devCfg);
//
// Read with the TCP/IP core locked (LWIP_TCPIP_CORE_LOCKING), else as
// plain reads of aligned words.
SocketNetDevice::Platform lwipNetifPlatform(struct netif* nif = nullptr);
