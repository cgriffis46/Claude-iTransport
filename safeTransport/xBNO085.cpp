#include "xBNO085.h"

xBNO085::xBNO085(iTransport& transport, size_t bufferSizeBytes,
                  size_t triggerLevelBytes, size_t frameQueueLength)
    : transport_(transport),
      streamBuffer_(xStreamBufferCreate(bufferSizeBytes, triggerLevelBytes)),
      frameQueue_(xQueueCreate(static_cast<UBaseType_t>(frameQueueLength), sizeof(ShtpFrame))) {
    // Safe here: this is a normal call on transport_, an already-fully-
    // constructed object — not a virtual dispatch on *this* before
    // *this* has finished constructing. See iTransport.h's own
    // comment on setRxSink() for why it's shaped this way at all.
    transport_.setRxSink(*this);
}

xBNO085::~xBNO085() {
    if (streamBuffer_) vStreamBufferDelete(streamBuffer_);
    if (frameQueue_)   vQueueDelete(frameQueue_);
}

// ISR context. xStreamBufferSendFromISR() is the FreeRTOS-native,
// ISR-safe way to push into a stream buffer — see this class's header
// comment for why native FreeRTOS is used here instead of CMSIS-RTOS2.
//
// portYIELD_FROM_ISR() matters here, not just as tidiness: if this
// send unblocks a higher-priority task (whatever's waiting in
// readBytes()), the ISR must request a context switch before
// returning, or that task won't actually run until the next tick —
// an easy detail to silently get wrong with FreeRTOS ISR-to-task
// signaling.
void xBNO085::onByteReceived(uint8_t byte) {
    BaseType_t higherPriorityTaskWoken = pdFALSE;
    xStreamBufferSendFromISR(streamBuffer_, &byte, 1, &higherPriorityTaskWoken);
    portYIELD_FROM_ISR(higherPriorityTaskWoken);
}

size_t xBNO085::readBytes(uint8_t* buf, size_t maxLen, uint32_t timeoutTicks) {
    return xStreamBufferReceive(streamBuffer_, buf, maxLen, timeoutTicks);
}

// Task context only — never called from an ISR, so the plain
// (non-FromISR) xQueueSend() is correct here, unlike onByteReceived()
// above. Copies the full ShtpFrame envelope (kMaxShtpFrameSize bytes)
// regardless of how short frame.length actually is — simple and safe
// (no buffer lifetime/ownership questions), at the cost of copying
// more than strictly necessary for small frames. Worth revisiting
// with a pointer/buffer-pool scheme later if that copy ever shows up
// as a real cost; unlikely at typical BNO085 report rates.
bool xBNO085::queueFrame(const ShtpFrame& frame, uint32_t timeoutTicks) {
    return xQueueSend(frameQueue_, &frame, timeoutTicks) == pdTRUE;
}

bool xBNO085::dequeueFrame(ShtpFrame& outFrame, uint32_t timeoutTicks) {
    return xQueueReceive(frameQueue_, &outFrame, timeoutTicks) == pdTRUE;
}

size_t xBNO085::processAvailableBytes(uint32_t timeoutTicks) {
    uint8_t chunk[32];
    const size_t n = readBytes(chunk, sizeof(chunk), timeoutTicks);
    for (size_t i = 0; i < n; ++i) {
        feedByte(chunk[i]);
    }
    return n;
}

void xBNO085::appendToFrame(uint8_t byte) {
    if (frameLen_ >= ShtpFrame::kMaxShtpFrameSize) {
        // Overflow — drop this frame-in-progress rather than corrupt
        // memory or silently truncate a frame and hand it to the main
        // thread looking valid. Wait for the next 0x7E to resync.
        assembling_ = false;
        frameLen_   = 0;
        return;
    }
    frameBuf_[frameLen_++] = byte;
}

// HDLC-style byte-stuffing: 0x7D escapes the next byte (real value is
// that byte XOR 0x20); 0x7E marks both the end of the frame just
// finished AND the start of whatever comes next — the same delimiter
// serves both roles, matching how BNO085's UART-SHTP framing works.
void xBNO085::feedByte(uint8_t byte) {
    if (escapeNext_) {
        escapeNext_ = false;
        appendToFrame(static_cast<uint8_t>(byte ^ 0x20));
        return;
    }

    if (byte == 0x7D) {
        escapeNext_ = true;
        return;
    }

    if (byte == 0x7E) {
        if (assembling_ && frameLen_ > 0) {
            // End of frame — hand it off unvalidated. NO CRC check
            // here, by design: that's the main thread's job once it
            // calls dequeueFrame(). Non-blocking queueFrame(): if the
            // main thread has fallen behind and the queue is full,
            // drop this frame rather than stall the framing task —
            // same "keep the time-critical path moving, prefer
            // dropping data over backing up the pipeline" reasoning
            // as onByteReceived()'s stream-buffer send.
            ShtpFrame frame;
            frame.length = frameLen_;
            for (size_t i = 0; i < frameLen_; ++i) frame.data[i] = frameBuf_[i];
            queueFrame(frame, 0);
        }
        // This 0x7E also serves as the start of whatever frame comes
        // next — reset and keep going, rather than requiring a
        // separate delimiter for each direction.
        assembling_ = true;
        frameLen_   = 0;
        escapeNext_ = false;
        return;
    }

    if (!assembling_) {
        return; // haven't seen a start-of-frame yet — ignore stray bytes
    }
    appendToFrame(byte);
}

void xBNO085::assembleFrames() {
    for (;;) {
        processAvailableBytes(portMAX_DELAY);
    }
}

void xBNO085::assemblerTaskTrampoline(void* arg) {
    static_cast<xBNO085*>(arg)->assembleFrames();
}

bool xBNO085::startFrameAssemblerTask(uint16_t stackDepthWords, UBaseType_t priority) {
    return xTaskCreate(&xBNO085::assemblerTaskTrampoline, "BNO085Framer",
                        stackDepthWords, this, priority, &assemblerTaskHandle_) == pdPASS;
}
