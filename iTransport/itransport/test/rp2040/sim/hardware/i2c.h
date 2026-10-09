#pragma once
// Stand-in for the Pico SDK's hardware/i2c.h. See pico_sim.h.
#include "../pico_sim.h"
#define I2C_IC_DATA_CMD_CMD_BITS          0x00000100u
#define I2C_IC_DATA_CMD_STOP_BITS         0x00000200u
#define I2C_IC_DATA_CMD_RESTART_BITS      0x00000400u
#define I2C_IC_INTR_MASK_M_RX_FULL_BITS   0x00000004u
#define I2C_IC_INTR_MASK_M_TX_EMPTY_BITS  0x00000010u
#define I2C_IC_INTR_MASK_M_TX_ABRT_BITS   0x00000040u
#define I2C_IC_INTR_MASK_M_STOP_DET_BITS  0x00000200u
#define I2C_IC_INTR_STAT_R_RX_FULL_BITS   I2C_IC_INTR_MASK_M_RX_FULL_BITS
#define I2C_IC_INTR_STAT_R_TX_EMPTY_BITS  I2C_IC_INTR_MASK_M_TX_EMPTY_BITS
#define I2C_IC_INTR_STAT_R_TX_ABRT_BITS   I2C_IC_INTR_MASK_M_TX_ABRT_BITS
#define I2C_IC_INTR_STAT_R_STOP_DET_BITS  I2C_IC_INTR_MASK_M_STOP_DET_BITS
struct i2c_hw_t { SimReg enable, tar, data_cmd, intr_mask, intr_stat, raw_intr_stat, rx_tl, tx_tl,
                         clr_intr, clr_tx_abrt, clr_stop_det, txflr, rxflr; };
struct i2c_inst_t { i2c_hw_t* hw; };
extern i2c_inst_t i2c0_inst, i2c1_inst;
#define i2c0 (&i2c0_inst)
#define i2c1 (&i2c1_inst)
static inline uint i2c_get_index(i2c_inst_t* i) { return i == i2c1 ? 1u : 0u; }
static inline i2c_hw_t* i2c_get_hw(i2c_inst_t* i) { return i->hw; }
int i2c_read_timeout_us(i2c_inst_t* i, uint8_t addr, uint8_t* dst, size_t len, bool nostop, uint timeout_us);
