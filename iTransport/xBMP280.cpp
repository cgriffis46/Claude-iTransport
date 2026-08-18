#include "xBMP280.h"
#include <cstddef>

void xBMP280::sleep(uint32_t ms) {
    osDelay(ms);
}

// Runs forever inside the newly spawned task — from here on, this is
// what drives main(), not whatever context originally called begin().
void xBMP280::threadTrampoline(void* arg) {
    auto* self = static_cast<xBMP280*>(arg);
    for (;;) {
        self->main(osKernelGetTickCount());
        self->sleep(1); // always yield at least one tick, even a pass where
                          // main() returns instantly (e.g. idling between polls)
    }
}

// osThreadNew() begins running the new task as soon as the scheduler
// is active (or immediately, if it already is) — CMSIS-RTOS2 has no
// separate "create" then "start" step.
bool xBMP280::startThread() {
    threadId_ = osThreadNew(&xBMP280::threadTrampoline, this, nullptr);
    return threadId_ != nullptr;
}

// Targets threadId_ specifically, not NULL — correct no matter which
// thread calls this, unlike relying on osThreadTerminate(NULL)'s
// "terminate the calling thread" convention. Does not return if it
// succeeds and threadId_ is the currently-running thread (self
// termination); if called to stop a DIFFERENT thread than the one
// currently running, it does return normally.
void xBMP280::StopThread() {
    osThreadTerminate(threadId_);
}
