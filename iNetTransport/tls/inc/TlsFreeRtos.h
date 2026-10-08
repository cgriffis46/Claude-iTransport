#pragma once
// FreeRTOS glue for MbedTlsServer and WebAuth: a mutex for each, and
// mbedTLS's heap on the FreeRTOS heap.
//
//     tlsUseFreeRtosHeap();                 // once, before anything else of mbedTLS
//     static TlsFreeRtosLock tlsLock, authLock;
#include <cstddef>
#include <cstring>
#include "FreeRTOS.h"
#include "semphr.h"
#include "mbedtls/platform.h"
#include "MbedTlsServer.h"
#include "WebAuth.h"

class TlsFreeRtosLock : public MbedTlsServer::Lock, public WebAuth::Lock {
public:
    TlsFreeRtosLock() : m_(xSemaphoreCreateMutex()) {}
    ~TlsFreeRtosLock() override { if (m_) vSemaphoreDelete(m_); }
    TlsFreeRtosLock(const TlsFreeRtosLock&) = delete;
    TlsFreeRtosLock& operator=(const TlsFreeRtosLock&) = delete;
    void lock() override { xSemaphoreTake(m_, portMAX_DELAY); }
    void unlock() override { xSemaphoreGive(m_); }
    bool ok() const { return m_ != nullptr; }

private:
    SemaphoreHandle_t m_;
};

inline void* tlsFreeRtosCalloc(size_t n, size_t size) {
    if (size && n > static_cast<size_t>(-1) / size) return nullptr;
    void* p = pvPortMalloc(n * size);
    if (p) std::memset(p, 0, n * size);
    return p;
}

// mbedTLS's calloc/free on pvPortMalloc/vPortFree: a TLS session's
// buffers (about 25 KB) then come from configTOTAL_HEAP_SIZE.
inline void tlsUseFreeRtosHeap() {
    mbedtls_platform_set_calloc_free(tlsFreeRtosCalloc, vPortFree);
}
