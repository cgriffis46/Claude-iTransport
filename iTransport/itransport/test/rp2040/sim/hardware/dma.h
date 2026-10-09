#pragma once
// Stand-in for the Pico SDK's hardware/dma.h. See pico_sim.h.
#include "../pico_sim.h"
enum dma_channel_transfer_size { DMA_SIZE_8 = 0, DMA_SIZE_16 = 1, DMA_SIZE_32 = 2 };
struct dma_channel_config { bool readInc = true, writeInc = true; uint dreq = 0; };
int  dma_claim_unused_channel(bool required);
void dma_channel_unclaim(uint channel);
static inline dma_channel_config dma_channel_get_default_config(uint channel) { (void)channel; return dma_channel_config(); }
static inline void channel_config_set_transfer_data_size(dma_channel_config* c, dma_channel_transfer_size s) { (void)c; (void)s; }
static inline void channel_config_set_read_increment(dma_channel_config* c, bool incr) { c->readInc = incr; }
static inline void channel_config_set_write_increment(dma_channel_config* c, bool incr) { c->writeInc = incr; }
static inline void channel_config_set_dreq(dma_channel_config* c, uint dreq) { c->dreq = dreq; }
void dma_channel_configure(uint channel, const dma_channel_config* config, volatile void* write_addr,
                           const volatile void* read_addr, uint32_t transfer_count, bool trigger);
void dma_start_channel_mask(uint32_t chan_mask);
static inline bool dma_channel_is_busy(uint channel) { return sim::dma[channel].busy; }
static inline void dma_channel_abort(uint channel) { sim::dma[channel].busy = false; }
static inline void dma_irqn_set_channel_enabled(uint irq_index, uint channel, bool enabled) { sim::dma[channel].irqLine[irq_index] = enabled; }
static inline bool dma_irqn_get_channel_status(uint irq_index, uint channel) { return sim::dma[channel].status[irq_index]; }
static inline void dma_irqn_acknowledge_channel(uint irq_index, uint channel) { sim::dma[channel].status[irq_index] = false; }
