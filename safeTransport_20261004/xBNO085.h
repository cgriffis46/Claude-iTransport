#pragma once
#include "FreeRTOS.h"
#include "stream_buffer.h"
#include "queue.h"
#include "task.h"
#include "iTransport.h"

// A fully-assembled SHTP frame, ready for the (not-yet-built)
// report-parsing layer to interpret. Fixed-size envelope rather than
// a variable-length allocation, so it fits a FreeRTOS queue's
// fixed-message-size requirement — actual content is only the first
// `length` bytes; the rest of `data` is unused padding for shorter
// frames. kMaxShtpFrameSize covers ordinary SHTP reports
// (advertisement, sensor reports, command responses) — BNO085's usual
// reports fit well within this. NOT sized for DFU-style continuation
// packets, which this scaffold doesn't handle at all yet. Adjust if a
// future report type needs more.
struct ShtpFrame {
    static constexpr size_t kMaxShtpFrameSize = 300;
    uint8_t data[kMaxShtpFrameSize];
    size_t  length = 0;
};

// FreeRTOS-backed BNO085 receiver scaffold. This is deliberately NOT
// yet a full BNO085 driver — no SHTP framing, no report parsing, none
// of that exists here yet. This exists specifically to settle the
// transport/buffering design: FreeRTOS's native stream buffer AND
// queue APIs (part of FreeRTOS itself, NOT part of the CMSIS-RTOS2
// spec) are confined entirely to this class, kept out of
// iTransport/Stm32HalUartTransport, which stay fully RTOS-agnostic.
// This is the one deliberate, scoped exception in this codebase to
// routing everything through CMSIS-RTOS2 — made here because a stream
// buffer is a genuinely better fit for a continuous byte stream than a
// message queue (see the byte-level receive path below), and having
// already committed to native FreeRTOS for that, using FreeRTOS's own
// queue for frame-level hand-off (rather than mixing in a CMSIS-RTOS2
// osMessageQueue here too) keeps this one class internally consistent.
//
// Two separate hand-offs live here, at two different granularities:
//   1. Byte-level: onByteReceived() (ISR) -> streamBuffer_ -> readBytes()
//      (called by whatever task assembles raw bytes into frames — not
//      yet built)
//   2. Frame-level: queueFrame() (called by that same frame-assembler
//      task, once it has a complete frame) -> frameQueue_ ->
//      dequeueFrame() (called by "the main thread" — the task that
//      actually interprets a complete SHTP frame's report data)
//
// A future pBNO085 would receive bytes completely differently —
// reading directly from a POSIX tty file descriptor, relying on the
// kernel's own serial buffering rather than needing an explicit
// userspace stream buffer at all ("use a file for the buffer" instead
// of this class's approach). Its frame-level hand-off would likely
// still look similar in shape (some queue-like mechanism), just built
// on POSIX primitives instead of FreeRTOS ones.
class xBNO085 : public iTransportRxSink {
public:
    // transport must outlive this object. triggerLevelBytes is how
    // many bytes must accumulate before a blocked readBytes() call
    // wakes — letting a future SHTP parser avoid waking for every
    // single byte once it knows how many it actually needs next (e.g.
    // the 4-byte SHTP header). frameQueueLength is how many complete
    // frames can be buffered before queueFrame() itself blocks,
    // waiting for the main thread to catch up.
    xBNO085(iTransport& transport, size_t bufferSizeBytes,
            size_t triggerLevelBytes, size_t frameQueueLength);
    ~xBNO085() override;

    // ISR context — called by the transport whenever a byte arrives.
    void onByteReceived(uint8_t byte) override;

    // Called from whatever task ends up parsing SHTP frames (not yet
    // built). Blocks up to timeoutTicks for at least one byte,
    // returning however many were actually available (up to maxLen).
    size_t readBytes(uint8_t* buf, size_t maxLen, uint32_t timeoutTicks);

    // Called by the frame-assembler task once it has a complete,
    // validated SHTP frame — task context, NOT ISR (frame assembly
    // involves too much work — delimiter scanning, un-escaping, CRC —
    // to ever run inside onByteReceived() itself). Copies the full
    // ShtpFrame envelope onto the queue; returns false if the queue
    // is full and stays full for the whole timeout (the main thread
    // has fallen behind).
    bool queueFrame(const ShtpFrame& frame, uint32_t timeoutTicks);

    // Called by the "main" consumer task to receive a complete,
    // assembled frame. Blocks up to timeoutTicks; returns false on
    // timeout with nothing available.
    bool dequeueFrame(ShtpFrame& outFrame, uint32_t timeoutTicks);

    // Convenience passthrough for sending — issues via the transport directly.
    bool write(const uint8_t* data, size_t len) { return transport_.write(data, len); }

    // Spawns a SECOND, dedicated FreeRTOS task whose only job is to
    // pull raw bytes (readBytes() above) and assemble them into
    // complete SHTP frames, pushing each one to frameQueue_ via
    // queueFrame() as soon as its end-of-frame delimiter is seen — NO
    // CRC validation happens here. That's deliberate: this task's only
    // responsibility is framing (finding 0x7E boundaries, un-escaping
    // 0x7D-stuffed bytes) fast enough to keep up with the UART; CRC
    // checking is deferred entirely to whatever "main thread" calls
    // dequeueFrame() (not yet built) — keeping this task simple and
    // fast, and keeping the heavier validation work off the time-
    // critical framing path. This can't share a thread with the main
    // consumer: reading the stream buffer and detecting frames is
    // continuous work, separate from whatever the main thread is
    // doing while waiting on dequeueFrame().
    bool startFrameAssemblerTask(uint16_t stackDepthWords, UBaseType_t priority);

    // Exposed for testing — does ONE pass of "read whatever bytes are
    // available (up to timeoutTicks), feed them through the framing
    // state machine." assembleFrames() below just calls this forever;
    // a test harness can call it directly, non-blocking, without
    // needing a real FreeRTOS task to be running.
    size_t processAvailableBytes(uint32_t timeoutTicks);

private:
    iTransport&       transport_;
    StreamBufferHandle_t  streamBuffer_;
    QueueHandle_t         frameQueue_;
    TaskHandle_t          assemblerTaskHandle_ = nullptr;

    // Framing state machine (HDLC-style byte-stuffing): 0x7E marks
    // start/end of a frame; 0x7D escapes the next byte (XOR 0x20 to
    // recover the real value). assembling_/frameLen_/escapeNext_ carry
    // state ACROSS calls to feedByte(), since a single UART byte
    // arrives at a time and a frame boundary can land anywhere.
    bool    assembling_  = false;
    bool    escapeNext_  = false;
    size_t  frameLen_    = 0;
    uint8_t frameBuf_[ShtpFrame::kMaxShtpFrameSize] = {0};

    void feedByte(uint8_t byte);
    void appendToFrame(uint8_t byte);
    void assembleFrames(); // the task body: for(;;) processAvailableBytes(portMAX_DELAY);
    static void assemblerTaskTrampoline(void* arg);
};
