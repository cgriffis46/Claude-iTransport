#include "xNetInterface.h"
#include "task.h"

xNetInterface::xNetInterface(iNetDevice& dev, const Config& cfg)
    : dev_(dev), cfg_(cfg) {
    dev_.attach(*this);
}

xNetInterface::~xNetInterface() {
    for (Slot& k : slots_) {
        if (k.rx) vStreamBufferDelete(k.rx);
        if (k.tx) vStreamBufferDelete(k.tx);
        if (k.ev) vEventGroupDelete(k.ev);
    }
    if (events_) vEventGroupDelete(events_);
    if (inbox_) vQueueDelete(inbox_);
}

TickType_t xNetInterface::toTicks(uint32_t ms) {
    return ms == kForever ? portMAX_DELAY : pdMS_TO_TICKS(ms);
}

uint32_t xNetInterface::nowMs() {
    return static_cast<uint32_t>(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

bool xNetInterface::begin(const NetConfig& net) {
    if (inbox_ != nullptr) return reconfigure(net);

    nSockets_ = cfg_.maxSockets;
    if (nSockets_ > dev_.socketCount()) nSockets_ = dev_.socketCount();
    if (nSockets_ > kMaxSockets) nSockets_ = kMaxSockets;

    inbox_  = xQueueCreate(cfg_.inboxDepth, sizeof(Msg));
    events_ = xEventGroupCreate();
    if (inbox_ == nullptr || events_ == nullptr) return false;
    xEventGroupSetBits(events_, kIfLinkDown);
    for (uint8_t s = 0; s < nSockets_; ++s) {
        Slot& k = slots_[s];
        k.rx = xStreamBufferCreate(cfg_.rxBufBytes, 1);
        k.tx = xStreamBufferCreate(cfg_.txBufBytes, 1);
        k.ev = xEventGroupCreate();
        if (k.rx == nullptr || k.tx == nullptr || k.ev == nullptr) return false;
        xEventGroupSetBits(k.ev, kEvClosed); // a socket nobody has opened is closed
    }
    return reconfigure(net);
}

bool xNetInterface::reconfigure(const NetConfig& net) {
    Msg m;
    m.op = Op::Configure;
    m.cfg = net;
    return post(m, 1000);
}

void xNetInterface::run() {
    configASSERT(inbox_ != nullptr); // begin() first
    for (;;) service(1000);
}

// The kick flag is cleared BEFORE the inbox is drained and the device
// polled. Data written after this point posts a fresh Kick, so the
// sleep at the end can't miss it; data written before it is seen by
// the poll.
void xNetInterface::service(uint32_t maxWaitMs) {
    kickPending_.store(false);

    Msg m;
    while (xQueueReceive(inbox_, &m, 0) == pdPASS) handle(m);
    if (irqPending_.exchange(false)) dev_.interrupt();

    uint32_t wait = dev_.poll(nowMs());
    if (wait > maxWaitMs) wait = maxWaitMs;
    if (wait == 0) return;

    TickType_t t = pdMS_TO_TICKS(wait);
    if (t == 0) t = 1; // a tick slower than 1 ms: still sleep, rather than spin
    if (xQueueReceive(inbox_, &m, t) == pdPASS) handle(m);
}

void xNetInterface::handle(const Msg& m) {
    switch (m.op) {
    case Op::Kick:
        break; // waking up was the point
    case Op::Configure:
        dev_.configure(m.cfg);
        break;
    case Op::Connect:
    case Op::Listen: {
        if (m.sock >= nSockets_) break;
        Slot& k = slots_[m.sock];
        // The owning user thread is waiting for kEvAccepted, not
        // touching the buffers, and nothing else uses them: safe to
        // empty. Any event the device raised about the socket's last
        // connection, before this, is wiped with them.
        xStreamBufferReset(k.rx);
        xStreamBufferReset(k.tx);
        k.rxStalled.store(false);
        xEventGroupClearBits(k.ev, kEvAll);
        const bool ok = (m.op == Op::Connect)
            ? dev_.connect(m.sock, m.ip, m.port, m.localPort)
            : dev_.listen(m.sock, m.localPort);
        if (!ok) socketEvent(m.sock, SocketEvent::Failed);
        // Last, so a refusal is already showing when the client wakes.
        xEventGroupSetBits(k.ev, kEvAccepted);
        break;
    }
    case Op::Close:
        if (m.sock < nSockets_) dev_.close(m.sock);
        break;
    default:
        handleOther(m);
        break;
    }
}

bool xNetInterface::post(const Msg& m, uint32_t timeoutMs) {
    if (inbox_ == nullptr) return false;
    return xQueueSend(inbox_, &m, toTicks(timeoutMs)) == pdPASS;
}

void xNetInterface::kick() {
    if (kickPending_.exchange(true)) return; // one is already on its way
    Msg m;
    // Full inbox: the driver thread has plenty to wake it already, and
    // clears kickPending_ when it does.
    (void)xQueueSend(inbox_, &m, 0);
}

void xNetInterface::interruptFromIsr() {
    if (inbox_ == nullptr) return;
    irqPending_.store(true);
    if (kickPending_.exchange(true)) return;
    Msg m;
    BaseType_t woken = pdFALSE;
    (void)xQueueSendFromISR(inbox_, &m, &woken);
    portYIELD_FROM_ISR(woken);
}

bool xNetInterface::ready() const {
    return events_ != nullptr && (xEventGroupGetBits(events_) & kIfReady) != 0;
}

bool xNetInterface::waitReady(uint32_t timeoutMs) {
    if (events_ == nullptr) return false;
    return (xEventGroupWaitBits(events_, kIfReady, pdFALSE, pdFALSE, toTicks(timeoutMs)) & kIfReady) != 0;
}

bool xNetInterface::linkUp() const {
    return events_ != nullptr && (xEventGroupGetBits(events_) & kIfLink) != 0;
}

bool xNetInterface::waitLinkUp(uint32_t timeoutMs) {
    if (events_ == nullptr) return false;
    return (xEventGroupWaitBits(events_, kIfLink, pdFALSE, pdFALSE, toTicks(timeoutMs)) & kIfLink) != 0;
}

// ---- sockets, for xClient ----

int xNetInterface::claim(xClient* owner) {
    int found = -1;
    taskENTER_CRITICAL();
    for (uint8_t s = 0; s < nSockets_; ++s) {
        if (slots_[s].owner == nullptr) {
            slots_[s].owner = owner;
            found = s;
            break;
        }
    }
    taskEXIT_CRITICAL();
    return found;
}

void xNetInterface::release(int s) {
    taskENTER_CRITICAL();
    slots_[s].owner = nullptr;
    taskEXIT_CRITICAL();
}

// ---- iNetDeviceHost: the driver thread ----

// The stall flag goes up before the space is read, so a reader that
// makes room in between sees it and kicks; see xClient::read().
size_t xNetInterface::rxSpace(uint8_t s) {
    if (s >= nSockets_) return 0;
    Slot& k = slots_[s];
    k.rxStalled.store(true);
    const size_t n = xStreamBufferSpacesAvailable(k.rx);
    if (n != 0) k.rxStalled.store(false);
    return n;
}

void xNetInterface::rxDeliver(uint8_t s, const uint8_t* data, size_t len) {
    if (s >= nSockets_) return;
    Slot& k = slots_[s];
    xStreamBufferSend(k.rx, data, len, 0); // fits: len <= rxSpace()
    xEventGroupSetBits(k.ev, kEvRx);
}

size_t xNetInterface::txPending(uint8_t s) {
    if (s >= nSockets_) return 0;
    return xStreamBufferBytesAvailable(slots_[s].tx);
}

size_t xNetInterface::txTake(uint8_t s, uint8_t* dst, size_t max) {
    if (s >= nSockets_) return 0;
    Slot& k = slots_[s];
    const size_t n = xStreamBufferReceive(k.tx, dst, max, 0);
    if (n != 0) xEventGroupSetBits(k.ev, kEvTx);
    return n;
}

void xNetInterface::socketEvent(uint8_t s, SocketEvent ev) {
    if (s >= nSockets_) return;
    Slot& k = slots_[s];
    switch (ev) {
    case SocketEvent::Listening:
        xEventGroupSetBits(k.ev, kEvListening);
        break;
    case SocketEvent::Connected:
        xEventGroupSetBits(k.ev, kEvConnected);
        break;
    case SocketEvent::Closed:
        xEventGroupClearBits(k.ev, kEvConnected);
        xEventGroupSetBits(k.ev, kEvClosed);
        break;
    case SocketEvent::Failed:
        xEventGroupClearBits(k.ev, kEvConnected);
        xEventGroupSetBits(k.ev, kEvFailed | kEvClosed);
        break;
    }
}

void xNetInterface::deviceEvent(DeviceEvent ev) {
    switch (ev) {
    case DeviceEvent::Ready:
        xEventGroupClearBits(events_, kIfFailed);
        xEventGroupSetBits(events_, kIfReady);
        break;
    case DeviceEvent::Failed:
        xEventGroupClearBits(events_, kIfReady);
        xEventGroupSetBits(events_, kIfFailed);
        break;
    case DeviceEvent::LinkUp:
        xEventGroupClearBits(events_, kIfJoinFailed | kIfLinkDown);
        xEventGroupSetBits(events_, kIfLink);
        break;
    case DeviceEvent::LinkDown:
        xEventGroupClearBits(events_, kIfLink);
        xEventGroupSetBits(events_, kIfLinkDown);
        break;
    case DeviceEvent::JoinFailed:
        xEventGroupSetBits(events_, kIfJoinFailed);
        break;
    }
}
