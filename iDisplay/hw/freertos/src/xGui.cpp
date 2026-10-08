#include "xGui.h"

namespace idisplay {

xGui::xGui(iDisplayDevice& display, xScreen& home, uint32_t queueDepth)
    : display_(display), core_(display.text(), home),
      queueDepth_(queueDepth == 0 ? 1 : queueDepth) {}

bool xGui::start(const char* name, uint32_t stackBytes, osPriority_t priority) {
    if (queue_ == nullptr) {
        queue_ = osMessageQueueNew(queueDepth_, sizeof(xGuiEvent), nullptr);
        if (queue_ == nullptr) return false;
    }
    if (thread_ != nullptr) return true;
    osThreadAttr_t attr = {};
    attr.name = name;
    attr.stack_size = stackBytes;
    attr.priority = priority;
    thread_ = osThreadNew(threadEntry, this, &attr);
    return thread_ != nullptr;
}

bool xGui::post(const xGuiEvent& ev, uint32_t timeoutTicks) {
    if (queue_ == nullptr) return false;
    return osMessageQueuePut(queue_, &ev, 0, timeoutTicks) == osOK;
}

void xGui::threadEntry(void* arg) {
    static_cast<xGui*>(arg)->run();
}

void xGui::begin() {
    core_.start();
    draw();
}

void xGui::run() {
    begin();
    for (;;) runOnce(osWaitForever);
}

bool xGui::runOnce(uint32_t timeoutTicks) {
    xGuiEvent ev;
    if (queue_ == nullptr) return false;
    if (osMessageQueueGet(queue_, &ev, nullptr, timeoutTicks) != osOK) return false;
    if (core_.handle(ev)) draw();
    return true;
}

// Draws the top screen and sends it. main() runs at least once, so a
// display that failed earlier is given its chance to come back (after
// its backoff); a display that fails now is left to retry on the next
// event rather than holding up the GUI.
void xGui::draw() {
    core_.render();
    display_.requestFlush();
    do {
        display_.main(osKernelGetTickCount());
    } while (!display_.idle() && !display_.failed());
}

bool xGuiButton::onEdge(bool down) {
    xGuiEvent ev;
    if (!button_.onLevel(down, osKernelGetTickCount(), ev)) return false;
    return gui_.post(ev, 0);
}

} // namespace idisplay
