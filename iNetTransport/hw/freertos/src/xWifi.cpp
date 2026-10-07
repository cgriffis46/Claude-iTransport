#include "xWifi.h"
#include <cstring>

static bool copyBounded(char* dst, const char* src, size_t max) {
    if (src == nullptr) return false;
    const size_t n = std::strlen(src);
    if (n > max) return false;
    std::memcpy(dst, src, n + 1);
    return true;
}

bool xWifi::join(const char* ssid, const char* passphrase, uint32_t timeoutMs) {
    if (events_ == nullptr) return false; // begin() first
    if (!copyBounded(ssid_, ssid, kMaxSsid)) return false;
    if (!copyBounded(pass_, passphrase ? passphrase : "", kMaxPassphrase)) return false;

    xEventGroupClearBits(events_, kIfLink | kIfJoinFailed);
    Msg m;
    m.op = Op::Join;
    if (!post(m, timeoutMs)) return false;
    const EventBits_t b = xEventGroupWaitBits(events_, kIfLink | kIfJoinFailed, pdFALSE, pdFALSE, toTicks(timeoutMs));
    return (b & kIfLink) != 0;
}

void xWifi::leave(uint32_t timeoutMs) {
    if (events_ == nullptr) return;
    Msg m;
    m.op = Op::Leave;
    if (!post(m, timeoutMs)) return;
    xEventGroupWaitBits(events_, kIfLinkDown, pdFALSE, pdFALSE, toTicks(timeoutMs));
}

// Driver thread. The device copies the strings before returning.
void xWifi::handleOther(const Msg& m) {
    switch (m.op) {
    case Op::Join:  wifi_.join(ssid_, pass_); break;
    case Op::Leave: wifi_.leave(); break;
    default: break;
    }
}
