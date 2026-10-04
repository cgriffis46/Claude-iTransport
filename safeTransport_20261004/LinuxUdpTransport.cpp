#include "LinuxUdpTransport.h"
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <cstdio>

LinuxUdpTransport::LinuxUdpTransport(const char* remoteHost, uint16_t remotePort, uint16_t localPort) {
    sockFd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockFd_ < 0) {
        std::perror("LinuxUdpTransport: socket() failed");
    }

    struct sockaddr_in localAddr{};
    localAddr.sin_family      = AF_INET;
    localAddr.sin_addr.s_addr = INADDR_ANY;
    localAddr.sin_port        = htons(localPort);
    if (bind(sockFd_, reinterpret_cast<struct sockaddr*>(&localAddr), sizeof(localAddr)) < 0) {
        std::perror("LinuxUdpTransport: bind() failed");
    }

    std::memset(&remoteAddr_, 0, sizeof(remoteAddr_));
    remoteAddr_.sin_family = AF_INET;
    remoteAddr_.sin_port   = htons(remotePort);
    inet_pton(AF_INET, remoteHost, &remoteAddr_.sin_addr);

    pthread_create(&recvThread_, nullptr, &LinuxUdpTransport::threadTrampoline, this);
}

LinuxUdpTransport::~LinuxUdpTransport() {
    running_ = false;
    // recvfrom() is blocked waiting for a packet — shutdown() forces
    // it to return with an error, unblocking the receiver thread so
    // pthread_join() below doesn't hang forever waiting for a packet
    // that may never arrive.
    shutdown(sockFd_, SHUT_RDWR);
    pthread_join(recvThread_, nullptr);
    close(sockFd_);
}

bool LinuxUdpTransport::write(const uint8_t* data, size_t len) {
    const ssize_t sent = sendto(sockFd_, data, len, 0,
                                 reinterpret_cast<struct sockaddr*>(&remoteAddr_), sizeof(remoteAddr_));
    return sent == static_cast<ssize_t>(len);
}

void LinuxUdpTransport::setRxSink(iTransportRxSink& sink) {
    rxSink_ = &sink;
}

void* LinuxUdpTransport::threadTrampoline(void* arg) {
    static_cast<LinuxUdpTransport*>(arg)->receiveLoop();
    return nullptr;
}

void LinuxUdpTransport::receiveLoop() {
    uint8_t buf[2048];
    while (running_.load()) {
        const ssize_t n = recvfrom(sockFd_, buf, sizeof(buf), 0, nullptr, nullptr);
        if (n <= 0) {
            break; // socket closed (shutdown() during destruction) or a real error
        }
        if (rxSink_) {
            for (ssize_t i = 0; i < n; ++i) {
                rxSink_->onByteReceived(buf[static_cast<size_t>(i)]);
            }
        }
    }
}
