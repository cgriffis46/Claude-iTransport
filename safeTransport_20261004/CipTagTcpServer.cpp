#include "CipTagTcpServer.h"
#include <cstring>

CipTagTcpServer::CipTagTcpServer(PlcTagRegistry& registry) : registry_(registry) {
    for (size_t i = 0; i < kMaxConnections; ++i) {
        slots_[i].server = this;
        slots_[i].selfIndex = i;
    }
}

size_t CipTagTcpServer::claimFreeSlot() {
    for (size_t i = 0; i < kMaxConnections; ++i) {
        if (!slots_[i].inUse) {
            slots_[i].inUse = true;
            slots_[i].generation++;
            slots_[i].frameAccumLen = 0;
            slots_[i].outgoingLen = 0;
            return i;
        }
    }
    return kMaxConnections; // none free -- caller enforces the 8-connection cap on this
}

void CipTagTcpServer::releaseSlot(size_t index) {
    ConnectionSlot& slot = slots_[index];
    if (slot.completedFrameStream) {
        vStreamBufferDelete(slot.completedFrameStream);
        slot.completedFrameStream = nullptr;
    }
    slot.inUse = false;
    slot.pcb = nullptr;
    slot.frameAccumLen = 0;
}

size_t CipTagTcpServer::findSlotForPcb(struct tcp_pcb* pcb) {
    // Retained only as a general utility; the callbacks below no
    // longer need it (each one identifies its own slot directly via
    // tcp_arg), since relying on pcb-matching alone was exactly what
    // made the original onError implementation unable to work at all.
    for (size_t i = 0; i < kMaxConnections; ++i) {
        if (slots_[i].inUse && slots_[i].pcb == pcb) return i;
    }
    return kMaxConnections;
}

bool CipTagTcpServer::start(uint16_t port) {
    readyQueue_ = xQueueCreate(kMaxConnections, sizeof(ReadyNotification));
    if (!readyQueue_) return false;

    struct tcp_pcb* pcb = tcp_new();
    if (!pcb) return false;

    if (tcp_bind(pcb, IP_ADDR_ANY, port) != ERR_OK) {
        tcp_close(pcb);
        return false;
    }

    struct tcp_pcb* listenPcb = tcp_listen(pcb);
    if (!listenPcb) {
        tcp_close(pcb);
        return false;
    }
    listenPcb_ = listenPcb;

    tcp_arg(listenPcb_, this); // accept callback has no slot yet -- this is the only use of a bare `this` arg
    tcp_accept(listenPcb_, &CipTagTcpServer::onAccept);

    // Stack size chosen generously: CipTagMessageCodec uses
    // std::vector internally, which allocates from the heap, not this
    // task's own stack -- but a working heap allocator (FreeRTOS's
    // heap_4.c, or newlib's own) must be configured in the build for
    // that to work at all on the actual target. Flagged here rather
    // than silently assumed, since it's a real build-configuration
    // requirement this code depends on.
    BaseType_t created = xTaskCreate(&CipTagTcpServer::workerTaskTrampoline, "CipTagWorker",
                                      /*stack words*/ 512, this, /*priority*/ tskIDLE_PRIORITY + 1,
                                      &workerTask_);
    return created == pdPASS;
}

err_t CipTagTcpServer::onAccept(void* arg, struct tcp_pcb* newpcb, err_t err) {
    CipTagTcpServer* self = static_cast<CipTagTcpServer*>(arg);
    if (err != ERR_OK || newpcb == nullptr || self == nullptr) {
        return ERR_VAL;
    }

    const size_t slotIndex = self->claimFreeSlot();
    if (slotIndex == kMaxConnections) {
        // At the 8-connection cap. Per lwIP's documented raw-API
        // convention, returning anything other than ERR_OK from an
        // accept callback means the connection is aborted -- so a
        // 9th client gets a clean, immediate refusal rather than
        // lwIP silently never processing it.
        tcp_abort(newpcb);
        return ERR_ABRT;
    }

    ConnectionSlot& slot = self->slots_[slotIndex];
    slot.pcb = newpcb;
    slot.completedFrameStream = xStreamBufferCreate(kStreamBufferCapacity, 1);
    if (!slot.completedFrameStream) {
        self->releaseSlot(slotIndex);
        tcp_abort(newpcb);
        return ERR_ABRT;
    }

    // Pass THIS SLOT's own address as arg, not the server -- see the
    // header's comment on ConnectionSlot::server/selfIndex for why:
    // lwIP's error callback carries no pcb reference at all, so each
    // connection's callbacks must be able to self-identify from arg
    // alone.
    tcp_arg(newpcb, &slot);
    tcp_recv(newpcb, &CipTagTcpServer::onRecv);
    tcp_sent(newpcb, &CipTagTcpServer::onSent);
    tcp_err(newpcb, &CipTagTcpServer::onError);

    return ERR_OK;
}

err_t CipTagTcpServer::onRecv(void* arg, struct tcp_pcb* tpcb, struct pbuf* p, err_t err) {
    ConnectionSlot* slotPtr = static_cast<ConnectionSlot*>(arg);
    if (slotPtr == nullptr || !slotPtr->inUse) {
        if (p) pbuf_free(p);
        return ERR_OK;
    }
    ConnectionSlot& slot = *slotPtr;
    CipTagTcpServer* self = slot.server;

    if (p == nullptr) {
        // Remote closed its end (FIN received). Clean shutdown: close
        // our side and release the slot.
        tcp_close(tpcb);
        self->releaseSlot(slot.selfIndex);
        return ERR_OK;
    }

    if (err != ERR_OK) {
        pbuf_free(p);
        return err;
    }

    // Copy the pbuf chain's bytes into this connection's plain
    // accumulator, bounded by its remaining capacity. A peer sending
    // more than this buffer can ever hold (a malformed or hostile
    // length field, or simply too much data) is treated as a protocol
    // violation, not something to silently truncate or overflow past.
    const size_t capacity = sizeof(slot.frameAccum);
    struct pbuf* cur = p;
    while (cur != nullptr) {
        const size_t spaceLeft = capacity - slot.frameAccumLen;
        if (cur->len > 0 && spaceLeft == 0) {
            tcp_abort(tpcb);
            pbuf_free(p);
            self->releaseSlot(slot.selfIndex);
            return ERR_ABRT;
        }
        const size_t take = (static_cast<size_t>(cur->len) < spaceLeft) ? cur->len : spaceLeft;
        std::memcpy(slot.frameAccum + slot.frameAccumLen, cur->payload, take);
        slot.frameAccumLen += take;
        cur = cur->next;
    }

    tcp_recved(tpcb, p->tot_len);
    pbuf_free(p);

    // Drain as many COMPLETE frames as are now available -- a peer
    // that sends several requests back-to-back without waiting for
    // each response could land more than one complete frame in a
    // single receive.
    while (slot.frameAccumLen >= CipFrame::kHeaderLen) {
        const uint16_t payloadLen = CipFrame::decodeHeader(slot.frameAccum);
        const size_t totalFrameLen = CipFrame::kHeaderLen + payloadLen;

        if (totalFrameLen > capacity) {
            // Declared length larger than this connection's buffer
            // could ever hold -- malformed/hostile length field, not
            // recoverable. Abort rather than wait forever.
            tcp_abort(tpcb);
            self->releaseSlot(slot.selfIndex);
            return ERR_ABRT;
        }

        if (slot.frameAccumLen < totalFrameLen) {
            break; // frame not fully arrived yet -- wait for more onRecv calls
        }

        // Hand exactly this one complete frame to the worker task.
        // Timeout 0: this runs in tcpip_thread and must never block.
        // If the worker hasn't drained a PREVIOUS frame on this same
        // connection yet, this send fails outright rather than
        // partially succeeding -- an accepted trade-off for this
        // workload (one request in flight per connection at a time).
        const size_t sent = xStreamBufferSend(slot.completedFrameStream, slot.frameAccum, totalFrameLen, 0);
        if (sent != totalFrameLen) {
            tcp_abort(tpcb);
            self->releaseSlot(slot.selfIndex);
            return ERR_ABRT;
        }

        ReadyNotification note{slot.selfIndex, slot.generation};
        xQueueSend(self->readyQueue_, &note, 0); // never block tcpip_thread here either

        const size_t remaining = slot.frameAccumLen - totalFrameLen;
        std::memmove(slot.frameAccum, slot.frameAccum + totalFrameLen, remaining);
        slot.frameAccumLen = remaining;
    }

    return ERR_OK;
}

err_t CipTagTcpServer::onSent(void* /*arg*/, struct tcp_pcb* /*tpcb*/, u16_t /*len*/) {
    // Nothing queued beyond a single in-flight response per
    // connection (see onRecv's own comment on that trade-off) -- no
    // further action needed here.
    return ERR_OK;
}

void CipTagTcpServer::onError(void* arg, err_t /*err*/) {
    // lwIP has ALREADY deallocated the pcb by the time this fires --
    // touching it further (tcp_close/tcp_abort) is undefined
    // behavior. Note lwIP's tcp_err_fn signature carries NO pcb at
    // all, only arg -- which is exactly why arg must be this slot's
    // own address (see header comment), not a shared server pointer;
    // with a shared pointer this callback would have no way to know
    // which of the 8 connections just failed.
    ConnectionSlot* slot = static_cast<ConnectionSlot*>(arg);
    if (!slot || !slot->inUse) return;
    slot->server->releaseSlot(slot->selfIndex);
}

void CipTagTcpServer::workerTaskTrampoline(void* context) {
    static_cast<CipTagTcpServer*>(context)->workerTaskLoop();
}

void CipTagTcpServer::workerTaskLoop() {
    for (;;) {
        ReadyNotification note;
        if (xQueueReceive(readyQueue_, &note, portMAX_DELAY) != pdPASS) {
            continue;
        }

        ConnectionSlot& slot = slots_[note.slotIndex];
        if (!slot.inUse || slot.generation != note.generation) {
            continue; // connection closed/reused since this was queued
        }

        uint8_t frame[CipFrame::kHeaderLen + CipFrame::kMaxPayloadLen];
        const size_t received = xStreamBufferReceive(slot.completedFrameStream, frame, sizeof(frame), 0);
        if (received < CipFrame::kHeaderLen) {
            continue; // shouldn't happen given onRecv only ever sends complete frames
        }

        const uint16_t payloadLen = CipFrame::decodeHeader(frame);
        std::vector<uint8_t> request(frame + CipFrame::kHeaderLen, frame + CipFrame::kHeaderLen + payloadLen);

        std::vector<uint8_t> response;
        CipTagMessageCodec::handleRequest(registry_, request, response);

        // Re-check generation again: handleRequest() can take
        // nontrivial time relative to a connection closing in the
        // background via a concurrent onError/onRecv(p==nullptr).
        if (!slot.inUse || slot.generation != note.generation) {
            continue;
        }

        const uint16_t responseLen = static_cast<uint16_t>(response.size());
        CipFrame::encodeHeader(responseLen, slot.outgoingBuffer);
        std::memcpy(slot.outgoingBuffer + CipFrame::kHeaderLen, response.data(), response.size());
        slot.outgoingLen = static_cast<uint16_t>(CipFrame::kHeaderLen + response.size());

        // tcp_write()/tcp_output() must run IN tcpip_thread -- marshal
        // the actual transmission there rather than calling them
        // directly from this task.
        tcpip_callback(&CipTagTcpServer::transmitResponseInTcpipThread, &slot);
    }
}

void CipTagTcpServer::transmitResponseInTcpipThread(void* context) {
    ConnectionSlot* slot = static_cast<ConnectionSlot*>(context);
    if (!slot->inUse || slot->pcb == nullptr) {
        return; // connection closed before this marshaled call ran
    }
    tcp_write(slot->pcb, slot->outgoingBuffer, slot->outgoingLen, TCP_WRITE_FLAG_COPY);
    tcp_output(slot->pcb);
}
