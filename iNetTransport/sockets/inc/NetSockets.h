#pragma once
// The few BSD socket calls SocketNetDevice makes, from lwIP's socket API
// (INET_SOCKETS_LWIP: an STM32 with its own Ethernet MAC, or an ESP32
// under ESP-IDF) or from the operating system's (Linux, macOS).
//
// lwIP: needs LWIP_SOCKET 1 (and so NO_SYS 0) in lwipopts.h. The lwip_
// names are called, and the functions here are named so that none of
// LWIP_COMPAT_SOCKETS's macros (socket(), close(), send(), ...) can
// touch them: any setting works, CubeMX's default 1 included.
#include <cstddef>
#include <cstdint>

#if defined(INET_SOCKETS_LWIP)
#include "lwip/sockets.h"
#include "lwip/errno.h"

// LWIP_COMPAT_SOCKETS 1 (lwIP's default, and CubeMX's) makes these
// macros, and iNetDevice's connect() and poll(), xClient's read() and
// write() and the like would be rewritten by them. Code that includes
// this header gets them undone; anywhere else, include lwIP's socket
// header after this project's headers, or set LWIP_COMPAT_SOCKETS 2
// (real functions instead of macros).
#undef accept
#undef bind
#undef shutdown
#undef getpeername
#undef getsockname
#undef setsockopt
#undef getsockopt
#undef closesocket
#undef connect
#undef listen
#undef recv
#undef recvmsg
#undef recvfrom
#undef send
#undef sendmsg
#undef sendto
#undef socket
#undef select
#undef poll
#undef ioctlsocket
#undef inet_ntop
#undef inet_pton
#undef read
#undef readv
#undef write
#undef writev
#undef close
#undef fcntl
#undef ioctl

namespace netsock {
typedef struct sockaddr_in SockAddrIn;
inline int  openSocket(int d, int t, int p) { return lwip_socket(d, t, p); }
inline int  closeSocket(int fd) { return lwip_close(fd); }
inline int  bindTo(int fd, const SockAddrIn& a) { return lwip_bind(fd, reinterpret_cast<const struct sockaddr*>(&a), sizeof a); }
inline int  listenOn(int fd, int backlog) { return lwip_listen(fd, backlog); }
inline int  acceptOn(int fd) { return lwip_accept(fd, nullptr, nullptr); }
inline int  connectTo(int fd, const SockAddrIn& a) { return lwip_connect(fd, reinterpret_cast<const struct sockaddr*>(&a), sizeof a); }
inline long recvSome(int fd, void* b, size_t n) { return lwip_recv(fd, b, n, 0); }
inline long sendSome(int fd, const void* b, size_t n) { return lwip_send(fd, b, n, 0); }
inline long sendTo(int fd, const void* b, size_t n, const SockAddrIn& a) {
    return lwip_sendto(fd, b, n, 0, reinterpret_cast<const struct sockaddr*>(&a), sizeof a);
}
inline long recvFrom(int fd, void* b, size_t n, SockAddrIn& from) {
    socklen_t len = sizeof from;
    return lwip_recvfrom(fd, b, n, 0, reinterpret_cast<struct sockaddr*>(&from), &len);
}
inline int  setNonBlocking(int fd) { return lwip_fcntl(fd, F_SETFL, O_NONBLOCK); }
inline int  setReuseAddr(int fd) { int one = 1; return lwip_setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one); }
inline int  setNoDelay(int fd) { int one = 1; return lwip_setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one); }
inline int  pendingError(int fd) {
    int err = 0;
    socklen_t len = sizeof err;
    if (lwip_getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0) return -1;
    return err;
}
inline int  selectNow(int maxFd, fd_set* r, fd_set* w) {
    struct timeval tv = {0, 0};
    return lwip_select(maxFd, r, w, nullptr, &tv);
}
inline int  lastError() { return errno; }
}  // namespace netsock

#else   // the operating system's sockets
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0   // macOS: SO_NOSIGPIPE would be the way; not needed for the tests
#endif

namespace netsock {
typedef struct sockaddr_in SockAddrIn;
inline int  openSocket(int d, int t, int p) { return ::socket(d, t, p); }
inline int  closeSocket(int fd) { return ::close(fd); }
inline int  bindTo(int fd, const SockAddrIn& a) { return ::bind(fd, reinterpret_cast<const struct sockaddr*>(&a), sizeof a); }
inline int  listenOn(int fd, int backlog) { return ::listen(fd, backlog); }
inline int  acceptOn(int fd) { return ::accept(fd, nullptr, nullptr); }
inline int  connectTo(int fd, const SockAddrIn& a) { return ::connect(fd, reinterpret_cast<const struct sockaddr*>(&a), sizeof a); }
inline long recvSome(int fd, void* b, size_t n) { return ::recv(fd, b, n, 0); }
inline long sendSome(int fd, const void* b, size_t n) { return ::send(fd, b, n, MSG_NOSIGNAL); }   // no SIGPIPE
inline long sendTo(int fd, const void* b, size_t n, const SockAddrIn& a) {
    return ::sendto(fd, b, n, MSG_NOSIGNAL, reinterpret_cast<const struct sockaddr*>(&a), sizeof a);
}
inline long recvFrom(int fd, void* b, size_t n, SockAddrIn& from) {
    socklen_t len = sizeof from;
    return ::recvfrom(fd, b, n, 0, reinterpret_cast<struct sockaddr*>(&from), &len);
}
inline int  setNonBlocking(int fd) { return ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL, 0) | O_NONBLOCK); }
inline int  setReuseAddr(int fd) { int one = 1; return ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one); }
inline int  setNoDelay(int fd) { int one = 1; return ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one); }
inline int  pendingError(int fd) {
    int err = 0;
    socklen_t len = sizeof err;
    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0) return -1;
    return err;
}
inline int  selectNow(int maxFd, fd_set* r, fd_set* w) {
    struct timeval tv = {0, 0};
    return ::select(maxFd, r, w, nullptr, &tv);
}
inline int  lastError() { return errno; }
}  // namespace netsock
#endif

namespace netsock {
inline bool wouldBlock(int e) { return e == EAGAIN || e == EWOULDBLOCK || e == EINTR; }
}  // namespace netsock
