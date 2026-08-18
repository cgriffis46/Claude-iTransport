#include "pBMP280.h"
#include <ctime>
#include <cerrno>
#include <csignal>

std::atomic<bool> pBMP280::s_signalReceived{false};

uint32_t pBMP280::monotonicMillis() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    // Wraps around a uint32_t like any other tick counter in this
    // codebase — the state machine's elapsed-time math already
    // handles unsigned wraparound correctly (see BMP280Sensor's own
    // comments on the same point).
    return static_cast<uint32_t>(ts.tv_sec) * 1000u +
           static_cast<uint32_t>(ts.tv_nsec / 1000000);
}

void pBMP280::sleep(uint32_t ms) {
    struct timespec req;
    req.tv_sec  = static_cast<time_t>(ms / 1000);
    req.tv_nsec = static_cast<long>(ms % 1000) * 1000000L;

    // nanosleep() can return early if interrupted by a signal
    // (EINTR), filling `req` in with however much time was left —
    // keep sleeping until the full duration has actually elapsed,
    // rather than treating any early return as "done."
    while (nanosleep(&req, &req) == -1 && errno == EINTR) {
        // req now holds the remaining time; loop and try again.
    }
}

void* pBMP280::threadTrampoline(void* arg) {
    auto* self = static_cast<pBMP280*>(arg);
    while (!self->stopRequested_.load() && !s_signalReceived.load()) {
        self->main(monotonicMillis());
        self->sleep(1); // always yield at least a tick, same reasoning as xBMP280's version
    }
    return nullptr;
}

// Signal-handler-safe: only a plain, lock-free atomic store, nothing
// else. (The C++ standard doesn't formally guarantee std::atomic is
// signal-safe the way plain `volatile sig_atomic_t` is — but a
// lock-free atomic<bool>'s store() is a single ordinary memory write
// on every mainstream platform/compiler, and is treated as
// signal-safe in practice by glibc, GCC, and Clang alike.)
void pBMP280::handleTerminationSignal(int /*signum*/) {
    s_signalReceived.store(true, std::memory_order_relaxed);
}

void pBMP280::installDefaultSignalHandlers() {
    struct sigaction sa{};
    sa.sa_handler = &pBMP280::handleTerminationSignal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT, &sa, nullptr);
}

bool pBMP280::startThread() {
    stopRequested_ = false;
    const int rc = pthread_create(&threadId_, nullptr, &pBMP280::threadTrampoline, this);
    threadStarted_ = (rc == 0);
    return threadStarted_;
}

void pBMP280::StopThread() {
    if (!threadStarted_) return; // nothing to stop — never started, or already stopped
    stopRequested_ = true;
    pthread_join(threadId_, nullptr); // blocks until threadTrampoline() actually notices and returns
    threadStarted_ = false;
}
