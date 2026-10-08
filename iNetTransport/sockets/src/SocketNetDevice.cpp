#include "SocketNetDevice.h"
#include <cstring>
#include "NetSockets.h"

namespace {

netsock::SockAddrIn sockAddr(const IpAddress& ip, uint16_t port) {
    netsock::SockAddrIn a;
    std::memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    std::memcpy(&a.sin_addr.s_addr, ip.b, 4);   // already network order
    return a;
}

bool isZero(const IpAddress& ip) { return ip == IpAddress(); }

}  // namespace

SocketNetDevice::SocketNetDevice(const Config& cfg) : cfg_(cfg) {
    if (cfg_.sockets > kMaxSockets) cfg_.sockets = kMaxSockets;
}

SocketNetDevice::~SocketNetDevice() {
    for (uint8_t s = 0; s < kMaxSockets; ++s) drop(s);
    for (Listener& l : listeners_) {
        if (l.fd >= 0) netsock::closeSocket(l.fd);
    }
    if (udp_ >= 0) netsock::closeSocket(udp_);
}

uint32_t SocketNetDevice::nextRand() {
    // xorshift32: query IDs and SNTP nonces need to be hard to guess
    // from outside, not cryptographic.
    rand_ ^= rand_ << 13;
    rand_ ^= rand_ >> 17;
    rand_ ^= rand_ << 5;
    return rand_;
}

void SocketNetDevice::configure(const NetConfig& cfg) {
    dropAll();
    net_ = cfg;
    configured_ = true;
    readySent_ = false;
    rand_ ^= (static_cast<uint32_t>(cfg.ip.b[3]) << 24) ^ (static_cast<uint32_t>(cfg.mac.b[5]) << 8) ^ cfg.mac.b[4];
    if (rand_ == 0) rand_ = 0x9E3779B9u;
}

// ---- events ----

// Events from requests go out at the next poll(), on the driver thread,
// after the request has been taken in.
void SocketNetDevice::queue(uint8_t s, SocketEvent ev) {
    if (eventCount_ < sizeof events_ / sizeof events_[0]) events_[eventCount_++] = Queued{s, ev};
}

// A new request on s: events still queued were about what s did before.
void SocketNetDevice::purge(uint8_t s) {
    uint8_t n = 0;
    for (uint8_t i = 0; i < eventCount_; ++i) {
        if (events_[i].s != s) events_[n++] = events_[i];
    }
    eventCount_ = n;
}

void SocketNetDevice::drop(uint8_t s) {
    Sock& k = socks_[s];
    if (k.st == St::Listening && k.listener < kMaxListeners) {
        Listener& l = listeners_[k.listener];
        if (l.users && --l.users == 0) l.idleSince = 0;   // timed from the next poll
    }
    if (k.fd >= 0) netsock::closeSocket(k.fd);
    k.fd = -1;
    k.st = St::Free;
    k.listener = 0xFF;
    k.pendLen = k.pendAt = 0;
}

void SocketNetDevice::lost(uint8_t s, SocketEvent ev) {
    drop(s);
    if (host_) host_->socketEvent(s, ev);
}

void SocketNetDevice::dropAll() {
    for (uint8_t s = 0; s < kMaxSockets; ++s) {
        if (socks_[s].st != St::Free) lost(s, SocketEvent::Failed);
    }
    for (Listener& l : listeners_) {
        if (l.fd >= 0) netsock::closeSocket(l.fd);
        l = Listener();
    }
    if (udp_ >= 0) netsock::closeSocket(udp_);
    udp_ = -1;
}

// ---- requests ----

bool SocketNetDevice::connect(uint8_t s, const IpAddress& ip, uint16_t port, uint16_t localPort) {
    if (!configured_ || s >= cfg_.sockets) return false;
    purge(s);
    drop(s);
    Sock& k = socks_[s];
    if (!addr_) {   // held until there is an address: fail now rather than hang
        queue(s, SocketEvent::Failed);
        return true;
    }
    k.fd = netsock::openSocket(AF_INET, SOCK_STREAM, 0);
    if (k.fd < 0) {
        queue(s, SocketEvent::Failed);
        return true;
    }
    netsock::setNonBlocking(k.fd);
    netsock::setNoDelay(k.fd);
    if (localPort && netsock::bindTo(k.fd, sockAddr(IpAddress(), localPort)) != 0) {
        drop(s);
        queue(s, SocketEvent::Failed);
        return true;
    }
    if (netsock::connectTo(k.fd, sockAddr(ip, port)) == 0) {
        k.st = St::Open;
        queue(s, SocketEvent::Connected);
        return true;
    }
    const int e = netsock::lastError();
    if (e == EINPROGRESS || netsock::wouldBlock(e)) {
        k.st = St::Connecting;
        return true;
    }
    drop(s);
    queue(s, SocketEvent::Failed);
    return true;
}

bool SocketNetDevice::listen(uint8_t s, uint16_t port) {
    if (!configured_ || s >= cfg_.sockets || port == 0) return false;
    purge(s);
    drop(s);
    uint8_t li = kMaxListeners, freeLi = kMaxListeners;
    for (uint8_t i = 0; i < kMaxListeners; ++i) {
        if (listeners_[i].fd >= 0 && listeners_[i].port == port) li = i;
        if (listeners_[i].fd < 0 && freeLi == kMaxListeners) freeLi = i;
    }
    if (li == kMaxListeners) {
        if (freeLi == kMaxListeners || !addr_) {
            queue(s, SocketEvent::Failed);
            return true;
        }
        const int fd = netsock::openSocket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            queue(s, SocketEvent::Failed);
            return true;
        }
        netsock::setReuseAddr(fd);
        if (netsock::bindTo(fd, sockAddr(IpAddress(), port)) != 0 || netsock::listenOn(fd, cfg_.listenBacklog) != 0) {
            netsock::closeSocket(fd);
            queue(s, SocketEvent::Failed);
            return true;
        }
        netsock::setNonBlocking(fd);
        li = freeLi;
        listeners_[li].fd = fd;
        listeners_[li].port = port;
        listeners_[li].users = 0;
    }
    ++listeners_[li].users;
    listeners_[li].idleSince = 0;
    socks_[s].st = St::Listening;
    socks_[s].listener = li;
    queue(s, SocketEvent::Listening);
    return true;
}

// The stack closes gracefully: what was sent still goes, then FIN.
void SocketNetDevice::close(uint8_t s) {
    if (s >= cfg_.sockets) return;
    purge(s);
    Sock& k = socks_[s];
    if (k.st == St::Open && k.pendLen > k.pendAt) netsock::sendSome(k.fd, k.pend + k.pendAt, k.pendLen - k.pendAt);
    drop(s);
    queue(s, SocketEvent::Closed);
}

bool SocketNetDevice::resolve(const char* name) {
    if (!configured_ || name == nullptr || std::strlen(name) > DnsClient::kMaxName + 1) return false;
    dns_.cancel();   // a new question replaces the old; its answer never comes
    std::strcpy(dnsName_, name);
    dnsReq_ = true;
    return true;
}

bool SocketNetDevice::requestTime(const char* server) {
    if (!configured_ || server == nullptr || std::strlen(server) > DnsClient::kMaxName + 1) return false;
    std::strcpy(ntpServer_, server);
    ntpReq_ = true;
    return true;
}

// ---- the interface: link and address ----

void SocketNetDevice::checkInterface(uint32_t nowMs) {
    if (!readySent_) {
        readySent_ = true;
        link_ = addr_ = false;
        have_ = NetConfig();
        if (host_) host_->deviceEvent(DeviceEvent::Ready);
    }
    const bool link = cfg_.platform.linkUp ? cfg_.platform.linkUp(cfg_.platform.ctx) : true;
    if (link != link_) {
        link_ = link;
        if (host_) host_->deviceEvent(link ? DeviceEvent::LinkUp : DeviceEvent::LinkDown);
    }
    NetConfig now = net_;
    bool addr = false;
    if (link) {
        if (cfg_.platform.address) {
            addr = cfg_.platform.address(now, cfg_.platform.ctx);
            if (isZero(now.dns)) now.dns = net_.dns;
        } else {
            addr = !isZero(net_.ip);
        }
    }
    if (addr && isZero(now.ip)) addr = false;
    if (addr != addr_ || (addr && (now.ip != have_.ip || now.subnet != have_.subnet || now.gateway != have_.gateway ||
                                   now.dns != have_.dns))) {
        const bool changed = addr_ && (!addr || now.ip != have_.ip);
        if (changed) {   // every socket goes with the old address
            dropAll();
            failQueries(nowMs);
        }
        addr_ = addr;
        have_ = addr ? now : NetConfig();
        if (!addr) have_.mac = net_.mac;
        if (host_) host_->addressChanged(have_);
    }
}

// ---- DNS and SNTP, on the device's UDP socket ----

void SocketNetDevice::udpSend(size_t len, const IpAddress& dst, uint16_t port) {
    if (len && udp_ >= 0) netsock::sendTo(udp_, buf_, len, sockAddr(dst, port));
}

void SocketNetDevice::failQueries(uint32_t nowMs) {
    if (dnsReq_ || dns_.busy()) {
        dnsReq_ = false;
        dns_.cancel();
        if (host_) host_->resolved(false, IpAddress());
    }
    if (ntpReq_ || ntpBusy_) {
        ntpReq_ = ntpBusy_ = false;
        ntpDns_.cancel();
        sntp_.cancel();
        if (host_) host_->timeReceived(false, 0, nowMs);
    }
}

void SocketNetDevice::serviceQueries(uint32_t nowMs) {
    if (udp_ < 0 && (dnsReq_ || ntpReq_)) {
        udp_ = netsock::openSocket(AF_INET, SOCK_DGRAM, 0);
        if (udp_ >= 0 && netsock::bindTo(udp_, sockAddr(IpAddress(), 0)) != 0) {
            netsock::closeSocket(udp_);
            udp_ = -1;
        }
        if (udp_ < 0) return failQueries(nowMs);
        netsock::setNonBlocking(udp_);
    }
    if (dnsReq_) {
        dnsReq_ = false;
        if (!dns_.begin(dnsName_, have_.dns, static_cast<uint16_t>(nextRand() >> 16), nowMs) && host_) {
            host_->resolved(false, IpAddress());
        }
    }
    if (ntpReq_) {
        ntpReq_ = false;
        ntpBusy_ = true;
        sntp_.cancel();
        ntpDns_.cancel();
        IpAddress ip;
        if (IpAddress::parse(ntpServer_, ip)) {
            if (!sntp_.begin(ip, nextRand(), nowMs)) { ntpBusy_ = false; if (host_) host_->timeReceived(false, 0, nowMs); }
        } else if (!ntpDns_.begin(ntpServer_, have_.dns, static_cast<uint16_t>(nextRand() >> 16), nowMs)) {
            ntpBusy_ = false;
            if (host_) host_->timeReceived(false, 0, nowMs);
        }
    }
    DnsClient::Action d = dns_.poll(nowMs, buf_, sizeof buf_);
    udpSend(d.len, d.dst, DnsClient::kServerPort);
    if (d.event != DnsClient::Event::None && host_) host_->resolved(d.event == DnsClient::Event::Resolved, dns_.address());
    d = ntpDns_.poll(nowMs, buf_, sizeof buf_);
    udpSend(d.len, d.dst, DnsClient::kServerPort);
    if (d.event == DnsClient::Event::Resolved && sntp_.begin(ntpDns_.address(), nextRand(), nowMs)) {
        d.event = DnsClient::Event::None;
    }
    if (d.event != DnsClient::Event::None) {
        ntpBusy_ = false;
        if (host_) host_->timeReceived(false, 0, nowMs);
    }
    const SntpClient::Action t = sntp_.poll(nowMs, buf_, sizeof buf_);
    udpSend(t.len, t.dst, SntpClient::kServerPort);
    if (t.event != SntpClient::Event::None) {
        ntpBusy_ = false;
        if (host_) host_->timeReceived(t.event == SntpClient::Event::Synced, sntp_.unixMs(), sntp_.atMs());
    }
}

// ---- poll ----

uint32_t SocketNetDevice::poll(uint32_t nowMs) {
    // Events from requests first, so they come before what follows them.
    for (uint8_t i = 0; i < eventCount_; ++i) {
        if (host_) host_->socketEvent(events_[i].s, events_[i].ev);
    }
    eventCount_ = 0;
    if (!configured_) return cfg_.idlePollMs;

    checkInterface(nowMs);
    if (!addr_) {
        failQueries(nowMs);
        return cfg_.idlePollMs;
    }
    serviceQueries(nowMs);

    // One select() over everything that might be ready.
    fd_set rd, wr;
    FD_ZERO(&rd);
    FD_ZERO(&wr);
    int maxFd = -1;
    auto watch = [&](int fd, fd_set& set) {
        FD_SET(fd, &set);
        if (fd > maxFd) maxFd = fd;
    };
    bool active = false;
    for (Listener& l : listeners_) {
        if (l.fd < 0) continue;
        if (l.users) {
            watch(l.fd, rd);
            active = true;
        } else if (l.idleSince == 0) {
            l.idleSince = nowMs | 1;
        } else if (nowMs - l.idleSince >= cfg_.listenLingerMs) {
            netsock::closeSocket(l.fd);   // nobody has listened on this port for a while
            l = Listener();
        }
    }
    for (uint8_t s = 0; s < cfg_.sockets; ++s) {
        Sock& k = socks_[s];
        if (k.st == St::Connecting) {
            watch(k.fd, wr);
            active = true;
        } else if (k.st == St::Open) {
            active = true;
            if (host_ && host_->rxSpace(s)) watch(k.fd, rd);
            if (k.pendLen > k.pendAt || (host_ && host_->txPending(s))) watch(k.fd, wr);
        }
    }
    if (udp_ >= 0) watch(udp_, rd);
    if (maxFd >= 0 && netsock::selectNow(maxFd + 1, &rd, &wr) > 0) {
        // New connections, for sockets listening on their port.
        for (uint8_t li = 0; li < kMaxListeners; ++li) {
            Listener& l = listeners_[li];
            if (l.fd < 0 || !FD_ISSET(l.fd, &rd)) continue;
            while (l.users) {
                const int fd = netsock::acceptOn(l.fd);
                if (fd < 0) break;
                netsock::setNonBlocking(fd);
                netsock::setNoDelay(fd);
                for (uint8_t s = 0; s < cfg_.sockets; ++s) {
                    Sock& k = socks_[s];
                    if (k.st != St::Listening || k.listener != li) continue;
                    if (--l.users == 0) l.idleSince = 0;
                    k.fd = fd;
                    k.st = St::Open;
                    k.listener = 0xFF;
                    if (host_) host_->socketEvent(s, SocketEvent::Connected);
                    break;
                }
            }
        }
        for (uint8_t s = 0; s < cfg_.sockets; ++s) {
            Sock& k = socks_[s];
            if (k.st == St::Connecting && FD_ISSET(k.fd, &wr)) {
                if (netsock::pendingError(k.fd) == 0) {
                    k.st = St::Open;
                    if (host_) host_->socketEvent(s, SocketEvent::Connected);
                } else {
                    lost(s, SocketEvent::Failed);   // refused, or no route
                }
                continue;
            }
            if (k.st != St::Open) continue;
            // Received: as much as the host has room for.
            if (FD_ISSET(k.fd, &rd)) {
                for (int i = 0; i < 8 && k.st == St::Open; ++i) {
                    size_t room = host_ ? host_->rxSpace(s) : 0;
                    if (room == 0) break;
                    if (room > sizeof buf_) room = sizeof buf_;
                    const long n = netsock::recvSome(k.fd, buf_, room);
                    if (n > 0) {
                        host_->rxDeliver(s, buf_, static_cast<size_t>(n));
                        if (static_cast<size_t>(n) < room) break;
                    } else if (n == 0) {
                        lost(s, SocketEvent::Closed);   // the peer closed: everything it sent is delivered
                    } else if (!netsock::wouldBlock(netsock::lastError())) {
                        lost(s, SocketEvent::Failed);
                    } else {
                        break;
                    }
                }
            }
            // To send: the host's bytes, a chunk at a time, until the stack's buffer is full.
            if (k.st == St::Open && FD_ISSET(k.fd, &wr)) {
                for (int i = 0; i < 16; ++i) {
                    if (k.pendAt == k.pendLen) {
                        k.pendAt = 0;
                        k.pendLen = host_ ? static_cast<uint16_t>(host_->txTake(s, k.pend, sizeof k.pend)) : 0;
                        if (k.pendLen == 0) break;
                    }
                    const long n = netsock::sendSome(k.fd, k.pend + k.pendAt, k.pendLen - k.pendAt);
                    if (n > 0) {
                        k.pendAt = static_cast<uint16_t>(k.pendAt + n);
                    } else {
                        if (!netsock::wouldBlock(netsock::lastError())) lost(s, SocketEvent::Failed);
                        break;
                    }
                }
            }
        }
        // DNS and SNTP answers, told apart by the port they come from.
        if (udp_ >= 0 && FD_ISSET(udp_, &rd)) {
            for (int i = 0; i < 4; ++i) {
                netsock::SockAddrIn from;
                const long n = netsock::recvFrom(udp_, buf_, sizeof buf_, from);
                if (n <= 0) break;
                const uint16_t port = ntohs(from.sin_port);
                if (port == DnsClient::kServerPort) {
                    const DnsClient::Event e1 = dns_.receive(buf_, static_cast<size_t>(n), nowMs).event;
                    if (e1 != DnsClient::Event::None && host_) host_->resolved(e1 == DnsClient::Event::Resolved, dns_.address());
                    const DnsClient::Event e2 = ntpDns_.receive(buf_, static_cast<size_t>(n), nowMs).event;
                    if (e2 == DnsClient::Event::Resolved && sntp_.begin(ntpDns_.address(), nextRand(), nowMs)) continue;
                    if (e2 != DnsClient::Event::None) {
                        ntpBusy_ = false;
                        if (host_) host_->timeReceived(false, 0, nowMs);
                    }
                } else if (port == SntpClient::kServerPort) {
                    const SntpClient::Event e = sntp_.receive(buf_, static_cast<size_t>(n), nowMs).event;
                    if (e != SntpClient::Event::None) {
                        ntpBusy_ = false;
                        if (host_) host_->timeReceived(e == SntpClient::Event::Synced, sntp_.unixMs(), sntp_.atMs());
                    }
                }
            }
        }
    }

    if (eventCount_) return 0;
    uint32_t wait = active ? cfg_.activePollMs : cfg_.idlePollMs;
    const uint32_t q[] = {dns_.nextWakeMs(nowMs), ntpDns_.nextWakeMs(nowMs), sntp_.nextWakeMs(nowMs)};
    for (uint32_t w : q) {
        if (w < wait) wait = w;
    }
    if ((dns_.busy() || ntpDns_.busy() || sntp_.busy()) && wait > cfg_.activePollMs) wait = cfg_.activePollMs;
    return wait;
}
