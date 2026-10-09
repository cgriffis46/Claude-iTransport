#pragma once
// Stand-in for the Pico SDK's hardware/spi.h. See pico_sim.h.
#include "../pico_sim.h"
#define SPI_SSPSR_TNF_BITS   0x00000002u
#define SPI_SSPSR_RNE_BITS   0x00000004u
#define SPI_SSPSR_BSY_BITS   0x00000010u
#define SPI_SSPICR_RORIC_BITS 0x00000001u
struct spi_hw_t { SimReg dr, sr, icr; };
struct spi_inst_t;
extern spi_hw_t sim_spi_hw[2];
#define spi0 ((spi_inst_t*)&sim_spi_hw[0])
#define spi1 ((spi_inst_t*)&sim_spi_hw[1])
static inline spi_hw_t* spi_get_hw(spi_inst_t* s) { return (spi_hw_t*)s; }
static inline uint spi_get_index(const spi_inst_t* s) { return (const spi_hw_t*)s == &sim_spi_hw[1] ? 1u : 0u; }
static inline uint spi_get_dreq(spi_inst_t* s, bool is_tx) { return 16u + spi_get_index(s) * 2u + (is_tx ? 0u : 1u); }
