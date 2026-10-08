#pragma once

// A mutex, for code shared between threads that mustn't depend on an
// RTOS: the file systems behind the web server, say. TlsFreeRtosLock
// (tls/) is one on FreeRTOS; a test uses std::mutex.
class iLock {
public:
    virtual ~iLock() = default;
    virtual void lock() = 0;
    virtual void unlock() = 0;
};

// Holds an iLock (if any) for a scope.
class iLockGuard {
public:
    explicit iLockGuard(iLock* l) : l_(l) { if (l_) l_->lock(); }
    ~iLockGuard() { if (l_) l_->unlock(); }
    iLockGuard(const iLockGuard&) = delete;
    iLockGuard& operator=(const iLockGuard&) = delete;

private:
    iLock* l_;
};
