#pragma once
#include <cstdint>
#include <cstddef>
#include "lwip/tcp.h"
#include "lwip/tcpip.h" // tcpip_callback() -- confirmed via real cross-compile that tcp.h alone does NOT declare this
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "stream_buffer.h"
#include "PlcTagRegistry.h"
#include "CipTagMessageCodec.h"
#include "CipFrame.h"

// A non-blocking CIP tag server for the STM32 Nucleo-F207ZG's onboard
// Ethernet (LAN8742 PHY via the F2's integrated MAC).
//
// VERIFICATION STATUS, stated directly rather than left to be assumed
// from the rest of this project's track record: unlike nearly
// everything else built so far, this file has NOT been compiled or
// run — this sandbox has no real STM32 HAL, FreeRTOS, or lwIP headers
// to check it against. It's written carefully against lwIP's raw TCP
// API and FreeRTOS's stream buffer/queue APIs, both long-stable,
// well-documented interfaces — but treat this as a careful first
// draft for real hardware bring-up, not something with the same
// confidence as the layer 1/2 code, which actually compiled and ran
// against real tests.
//
// ARCHITECTURE — why each piece exists:
//
// lwIP's raw TCP API (tcp_accept/tcp_recv/tcp_sent/tcp_err/tcp_write)
// runs its callbacks from within tcpip_thread. Nothing in a raw-API
// callback may block — doing so stalls TCP/IP processing for every
// connection on the device, not just the one being serviced. But
// CipTagMessageCodec::handleRequest() ultimately calls into
// PlcTagRegistry, which uses a real mutex (std::mutex, or its
// FreeRTOS-backed equivalent) and must be free to block briefly. So
// the actual request processing cannot happen inside a raw-API
// callback — it has to happen in a normal FreeRTOS task, with the
// callback only handing data off to it:
//
//   ETH RX interrupt -> (lwIP's own standard driver/tcpip_thread
//     pipeline, not rewritten here) -> our tcp_recv callback
//     (tcpip_thread context, must not block)
//       -> raw bytes pushed into THIS CONNECTION's own stream buffer
//       -> once a complete framed message has accumulated, this
//          connection's slot index is pushed into ONE SHARED queue
//            -> a dedicated worker task blocks on that queue (free to
//               block — it's a normal task, not tcpip_thread), pulls
//               the slot, decodes the frame, calls
//               CipTagMessageCodec::handleRequest(), re-frames the
//               response
//                 -> tcpip_callback() marshals transmission back into
//                    tcpip_thread, where tcp_write()/tcp_output()
//                    actually run (raw-API calls must themselves run
//                    IN tcpip_thread, not from an arbitrary task)
//
// Each connection gets its OWN stream buffer (not one shared across
// connections) deliberately: a stream buffer is a single byte stream
// with no message-boundary awareness of its own, so sharing one
// across multiple simultaneous connections would interleave their
// bytes and corrupt message framing. One per connection costs more
// RAM (kMaxConnections of them) but is correct by construction rather
// than needing a separate demultiplexing scheme on top.
//
// One SHARED worker task (not one per connection) processes every
// connection's requests, serialized through one queue. This is a
// deliberate choice given this specific workload, not a default:
// CIP tag reads/writes here are tiny (a few bytes under a mutex) and
// fast, so serializing them costs negligible latency, and it avoids
// paying for kMaxConnections separate task stacks on a RAM-
// constrained F2-series MCU. If request processing ever became
// heavier, per-connection worker tasks would be the natural next
// step — not implemented here because this workload doesn't need it.
class CipTagTcpServer {
public:
    static constexpr size_t kMaxConnections = 8;
    static constexpr size_t kStreamBufferCapacity = CipFrame::kHeaderLen + CipFrame::kMaxPayloadLen;

    // registry must outlive this server. Does not start listening —
    // call start().
    explicit CipTagTcpServer(PlcTagRegistry& registry);

    // Creates the listening pcb, binds to port, starts the worker
    // task.
    //
    // CONFIRMED call site, verified directly against this project's
    // own CubeMX-generated LWIP/App/lwip.c, not assumed: call this
    // from MX_LWIP_Init()'s "USER CODE BEGIN 3" section — the very
    // end of that function, after tcpip_init(), netif_add(),
    // netif_set_default(), netif_set_up(), the link-status callback/
    // thread setup, and dhcp_start() have all already run. This is
    // the exact spot ST's own generated code reserves for
    // application-level setup needing the netif already up, and
    // direct raw-API calls here (not marshaled via tcpip_callback)
    // match ST's own reference pattern, since this runs before the
    // scheduler has concurrent tcpip_thread traffic interleaving with
    // it. Note dhcp_start() having just run means DHCP negotiation is
    // only STARTING, not necessarily complete, by the time this runs
    // — harmless here since tcp_bind() uses IP_ADDR_ANY rather than a
    // specific address, so it doesn't need an assigned IP yet.
    // Returns false if the listening pcb or worker task couldn't be
    // created.
    bool start(uint16_t port);

private:
    // Per-connection state. A fixed array of kMaxConnections of
    // these, never dynamically allocated — slots are claimed/released
    // as connections come and go, matching this project's established
    // "no heap allocation in anything embedded-resident" convention.
    struct ConnectionSlot {
        // Set once, at construction (server) and at claim time
        // (selfIndex) — never changes afterward. This slot's OWN
        // address is what gets passed as tcp_arg() for its
        // connection, specifically so lwIP's error callback (whose
        // signature carries NO pcb reference at all, only arg, since
        // the pcb is already freed by the time it fires) can still
        // identify which connection failed. Sharing one `this`-style
        // arg across every connection, as an earlier version of this
        // class did, would make every error callback indistinguishable
        // from every other — a real bug, not a style choice.
        CipTagTcpServer* server = nullptr;
        size_t           selfIndex = 0;

        bool            inUse = false;
        struct tcp_pcb* pcb = nullptr;

        // Incremented every time this slot is claimed for a new
        // connection (see claimFreeSlot()). Captured by the worker
        // task alongside the slot index, and re-checked by
        // transmitResponseInTcpipThread() before it touches pcb —
        // the worker task's processing and the marshaled transmit
        // happen asynchronously enough later that, in principle, this
        // slot could have been released and reused by a DIFFERENT
        // connection in between. Without this check, a stale response
        // could get written to the wrong, unrelated connection.
        uint32_t generation = 0;

        // Plain accumulator for incoming bytes, touched ONLY from
        // tcpip_thread (inside onRecv) — deliberately NOT a FreeRTOS
        // stream buffer, since FreeRTOS stream buffers have no way to
        // peek at accumulated bytes without consuming them, and
        // deciding "is a complete frame here yet" requires reading the
        // length header before committing to consume anything.
        uint8_t  frameAccum[CipFrame::kHeaderLen + CipFrame::kMaxPayloadLen];
        size_t   frameAccumLen = 0;

        // Only once onRecv has a COMPLETE frame does it hand those
        // exact bytes to the worker task, via this stream buffer —
        // this is the actual tcpip_thread-to-worker-task handoff.
        StreamBufferHandle_t completedFrameStream = nullptr;

        // Filled in by the worker task, consumed by the tcpip_thread-
        // marshaled transmit callback below — this connection's
        // outgoing bytes live here rather than on the worker task's
        // own stack, since tcpip_callback()'s marshaled function runs
        // later, asynchronously, after the worker task has moved on.
        uint8_t  outgoingBuffer[CipFrame::kHeaderLen + CipFrame::kMaxPayloadLen];
        uint16_t outgoingLen = 0;
    };

    // --- lwIP raw API callbacks (tcpip_thread context; must not block) ---
    static err_t onAccept(void* arg, struct tcp_pcb* newpcb, err_t err);
    static err_t onRecv(void* arg, struct tcp_pcb* tpcb, struct pbuf* p, err_t err);
    static err_t onSent(void* arg, struct tcp_pcb* tpcb, u16_t len);
    static void  onError(void* arg, err_t err);

    // Runs once per completed, fully-framed request — called from
    // onRecv() once a connection's stream buffer holds a complete
    // frame. Still tcpip_thread context: only ever pushes the slot
    // index into the shared queue, never touches the registry
    // directly.
    void notifyRequestReady(size_t slotIndex);

    // The one shared worker task — the only place
    // CipTagMessageCodec::handleRequest() is ever called from.
    static void workerTaskTrampoline(void* context);
    void workerTaskLoop();

    // Marshaled back into tcpip_thread via tcpip_callback() by the
    // worker task, once a response is ready — this is where
    // tcp_write()/tcp_output() actually run.
    static void transmitResponseInTcpipThread(void* context);

    // Finds a free slot (inUse == false) and claims it, or returns
    // kMaxConnections if none are free — the 8-connection cap is
    // enforced entirely by this running out of slots.
    size_t claimFreeSlot();
    void releaseSlot(size_t index);
    // Finds the slot index owning a given pcb, or kMaxConnections if
    // not found (e.g. already released by a concurrent error/close).
    size_t findSlotForPcb(struct tcp_pcb* pcb);

    // What actually travels through readyQueue_ — slot index plus the
    // generation it was valid for at notification time, so the
    // worker task (and later, the marshaled transmit callback) can
    // detect a slot that's since been released and reused.
    struct ReadyNotification {
        size_t   slotIndex;
        uint32_t generation;
    };

    PlcTagRegistry& registry_;
    struct tcp_pcb* listenPcb_ = nullptr;
    QueueHandle_t    readyQueue_ = nullptr; // carries ReadyNotification values
    TaskHandle_t     workerTask_ = nullptr;
    ConnectionSlot   slots_[kMaxConnections];
};
