#include "PicoI2CTransport.h"
#include "hardware/irq.h"

namespace {

// One transaction in flight on one I2C block. The I2C interrupt feeds
// it into the controller and collects what comes back.
//
// The controller takes commands, one per FIFO entry: a byte to write,
// or a request to read one, each optionally with RESTART (a repeated
// start before it) and STOP (after it). The whole transaction is
// queued as such commands: [reg] [writes...] [reads...], with RESTART
// on the first read when something was written before it, and STOP on
// the last command of all.
struct Engine {
    i2c_inst_t*    i2c        = nullptr;
    volatile bool  active     = false;
    bool           irqInstalled = false;

    bool           hasReg     = false;
    uint8_t        reg        = 0;
    const uint8_t* writeData  = nullptr;
    uint16_t       writeLen   = 0;
    uint8_t*       readData   = nullptr;
    uint16_t       readLen    = 0;

    uint16_t       pushed     = 0;  // commands queued so far
    uint16_t       received   = 0;  // bytes read back so far
    bool           aborted    = false;

    uint16_t writes() const { return static_cast<uint16_t>((hasReg ? 1 : 0) + writeLen); }
    uint16_t total() const  { return static_cast<uint16_t>(writes() + readLen); }
};

constexpr unsigned kBlocks    = NUM_I2CS;
constexpr uint32_t kFifoDepth = 16;
Engine s_engine[kBlocks];

constexpr uint32_t kMaskRunning = I2C_IC_INTR_MASK_M_TX_EMPTY_BITS | I2C_IC_INTR_MASK_M_RX_FULL_BITS |
                                  I2C_IC_INTR_MASK_M_TX_ABRT_BITS  | I2C_IC_INTR_MASK_M_STOP_DET_BITS;

void irq0() { PicoI2CTransport::handleIrq(0); }
void irq1() { PicoI2CTransport::handleIrq(1); }

// Collects whatever the controller has read.
void drain(Engine& e, i2c_hw_t* hw) {
    while (hw->rxflr > 0) {
        const uint32_t d = hw->data_cmd;
        if (e.received < e.readLen) e.readData[e.received] = static_cast<uint8_t>(d & 0xFF);
        ++e.received;
    }
}

// True if another command could be queued now: everything left is
// a write, or there is room in the receive FIFO for another read.
bool canQueue(const Engine& e) {
    if (e.pushed >= e.total()) return false;
    if (e.pushed < e.writes()) return true;
    const uint16_t readsQueued = static_cast<uint16_t>(e.pushed - e.writes());
    return static_cast<uint32_t>(readsQueued - e.received) < kFifoDepth;
}

// Queues as many commands as the FIFO will take. Never asks for more
// reads than the 16 entry receive FIFO can hold before they are
// collected, so it cannot overflow. Then asks for TX_EMPTY only if
// there is more it could queue: otherwise, while it waits for reads to
// come back, an empty FIFO would raise the interrupt again and again.
void fill(Engine& e, i2c_hw_t* hw) {
    const uint16_t writes = e.writes();
    const uint16_t total  = e.total();
    while (canQueue(e) && hw->txflr < kFifoDepth) {
        uint32_t cmd;
        if (e.pushed < writes) {
            const uint16_t i = e.pushed;
            cmd = (e.hasReg && i == 0) ? e.reg : e.writeData[i - (e.hasReg ? 1 : 0)];
        } else {
            cmd = I2C_IC_DATA_CMD_CMD_BITS;
            if (e.pushed == writes && writes > 0) cmd |= I2C_IC_DATA_CMD_RESTART_BITS;
        }
        if (e.pushed == total - 1) cmd |= I2C_IC_DATA_CMD_STOP_BITS;
        hw->data_cmd = cmd;
        ++e.pushed;
    }
    hw->intr_mask = canQueue(e) ? kMaskRunning : (kMaskRunning & ~I2C_IC_INTR_MASK_M_TX_EMPTY_BITS);
}

} // namespace

PicoI2CTransport::PicoI2CTransport(i2c_inst_t* i2c, uint8_t deviceAddr7bit, mutex_t* busMutex)
    : PicoSyncTransport<I2CTransport>(i2c, deviceAddr7bit, busMutex) {
    const unsigned index = i2c_get_index(i2c);
    Engine& e = s_engine[index];
    e.i2c = i2c;
    // The block may not be out of reset yet, so only the interrupt is
    // set up here. The handler masks the block's interrupts whenever
    // no transfer is running, which also quietens the reset default.
    if (!e.irqInstalled) {
        const unsigned irq = I2C0_IRQ + index;
        irq_add_shared_handler(irq, index == 0 ? irq0 : irq1, PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY);
        irq_set_enabled(irq, true);
        e.irqInstalled = true;
    }
}

bool PicoI2CTransport::start(bool hasReg, uint8_t reg, const uint8_t* writeData, uint16_t writeLen,
                             uint8_t* readData, uint16_t readLen) {
    Engine& e = s_engine[i2c_get_index(i2c())];
    if (e.active) return false;
    if ((hasReg ? 1 : 0) + writeLen + readLen == 0) return false;

    i2c_hw_t* hw = i2c_get_hw(i2c());

    // The target address can only be changed with the block disabled.
    hw->enable = 0;
    hw->tar = static_cast<uint32_t>(devAddr8bit_ >> 1);
    hw->enable = 1;
    const uint32_t ack = hw->clr_intr;   // clear anything left from before
    (void)ack;
    hw->rx_tl = 0;                       // RX_FULL from the first byte
    hw->tx_tl = kFifoDepth / 2;          // TX_EMPTY at half full

    e.hasReg    = hasReg;
    e.reg       = reg;
    e.writeData = writeData;
    e.writeLen  = writeLen;
    e.readData  = readData;
    e.readLen   = readLen;
    e.pushed    = 0;
    e.received  = 0;
    e.aborted   = false;
    e.active    = true;

    // From here the interrupt does the rest: TX_EMPTY is already
    // asserted, so it fires and starts queueing at once.
    hw->intr_mask = kMaskRunning;
    return true;
}

void PicoI2CTransport::handleIrq(unsigned index) {
    Engine& e = s_engine[index];
    if (e.i2c == nullptr) return;
    i2c_hw_t* hw = i2c_get_hw(e.i2c);

    if (!e.active) {
        hw->intr_mask = 0;
        return;
    }

    const uint32_t stat = hw->intr_stat;

    // A NACK, or lost arbitration: the controller has flushed what was
    // queued and is sending STOP. Queue nothing more, and finish when
    // STOP_DET arrives. The FIFO stays flushed until this is read.
    if (stat & I2C_IC_INTR_STAT_R_TX_ABRT_BITS) {
        e.aborted = true;
        e.pushed = e.total();
        const uint32_t ack = hw->clr_tx_abrt;
        (void)ack;
        hw->intr_mask = kMaskRunning & ~I2C_IC_INTR_MASK_M_TX_EMPTY_BITS;
    }

    drain(e, hw);

    // Collecting reads may have made room for more, so this is not
    // only for TX_EMPTY.
    if (!e.aborted) fill(e, hw);

    if (stat & I2C_IC_INTR_STAT_R_STOP_DET_BITS) {
        const uint32_t ack = hw->clr_stop_det;
        (void)ack;
        drain(e, hw);
        hw->intr_mask = 0;
        const bool failed = e.aborted || e.received != e.readLen;
        e.active = false;
        BusTransport::onTransferComplete(e.i2c, failed);
    }
}

bool PicoI2CTransport::halMemWrite(uint8_t reg, uint8_t* pData, uint16_t size) {
    return start(true, reg, pData, size, nullptr, 0);
}

bool PicoI2CTransport::halMemRead(uint8_t reg, uint8_t* pData, uint16_t size) {
    return start(true, reg, nullptr, 0, pData, size);
}

bool PicoI2CTransport::halMasterTransmit(uint8_t* pData, uint16_t size) {
    return start(false, 0, pData, size, nullptr, 0);
}

bool PicoI2CTransport::halMasterReceive(uint8_t* pData, uint16_t size) {
    return start(false, 0, nullptr, 0, pData, size);
}

// Blocks for up to trials x timeout ms. Not while a transfer of ours
// is running: the SDK's blocking calls would take its interrupts.
bool PicoI2CTransport::halIsDeviceReady(uint32_t trials, uint32_t timeout) {
    if (s_engine[i2c_get_index(i2c())].active) return false;
    for (uint32_t i = 0; i < trials; ++i) {
        uint8_t byte;
        if (i2c_read_timeout_us(i2c(), static_cast<uint8_t>(devAddr8bit_ >> 1), &byte, 1, false,
                                static_cast<unsigned>(timeout * 1000)) == 1) {
            return true;
        }
    }
    return false;
}
