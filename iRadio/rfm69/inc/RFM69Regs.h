/*
 * RFM69Regs.h
 *
 *  The SX1231 / RFM69 registers and bits the drivers here use, from the
 *  Semtech SX1231 datasheet (rev 7). Not a full map. Register numbers
 *  are plain (bit 7 clear); SPITransport::AddressBit::WriteHigh puts
 *  the write bit on.
 */

#ifndef RFM69REGS_H_
#define RFM69REGS_H_

#define RFM69_REG_FIFO           0x00
#define RFM69_REG_OPMODE         0x01
#define RFM69_REG_DATAMODUL      0x02
#define RFM69_REG_BITRATEMSB     0x03
#define RFM69_REG_BITRATELSB     0x04
#define RFM69_REG_FDEVMSB        0x05
#define RFM69_REG_FDEVLSB        0x06
#define RFM69_REG_FRFMSB         0x07	// 0x07-0x09: carrier frequency, in 61.035 Hz steps
#define RFM69_REG_FRFMID         0x08
#define RFM69_REG_FRFLSB         0x09
#define RFM69_REG_AFCCTRL        0x0B
#define RFM69_REG_VERSION        0x10
#define RFM69_REG_LNA            0x18
#define RFM69_REG_RXBW           0x19
#define RFM69_REG_AFCBW          0x1A
#define RFM69_REG_AFCFEI         0x1E
#define RFM69_REG_FEIMSB         0x21	// 0x21-0x22: frequency error, signed, 61.035 Hz steps
#define RFM69_REG_FEILSB         0x22
#define RFM69_REG_RSSICONFIG     0x23
#define RFM69_REG_RSSIVALUE      0x24	// -value / 2 dBm
#define RFM69_REG_DIOMAPPING1    0x25
#define RFM69_REG_DIOMAPPING2    0x26
#define RFM69_REG_IRQFLAGS1      0x27
#define RFM69_REG_IRQFLAGS2      0x28
#define RFM69_REG_RSSITHRESH     0x29	// -value / 2 dBm
#define RFM69_REG_PREAMBLEMSB    0x2C
#define RFM69_REG_PREAMBLELSB    0x2D
#define RFM69_REG_SYNCCONFIG     0x2E
#define RFM69_REG_SYNCVALUE1     0x2F
#define RFM69_REG_SYNCVALUE2     0x30
#define RFM69_REG_PACKETCONFIG1  0x37
#define RFM69_REG_PAYLOADLENGTH  0x38
#define RFM69_REG_FIFOTHRESH     0x3C
#define RFM69_REG_PACKETCONFIG2  0x3D
#define RFM69_REG_TESTDAGC       0x6F
#define RFM69_REG_TESTAFC        0x71

#define RFM69_VERSION            0x24	// RegVersion of the SX1231H in the RFM69

// RegOpMode: sequencer on, listen off, and the mode in bits 4-2
#define RFM69_OPMODE_SLEEP       0x00
#define RFM69_OPMODE_STANDBY     0x04
#define RFM69_OPMODE_SYNTH       0x08
#define RFM69_OPMODE_TX          0x0C
#define RFM69_OPMODE_RX          0x10

// RegIrqFlags1 / 2
#define RFM69_IRQ1_MODEREADY     0x80
#define RFM69_IRQ1_RXREADY       0x40
#define RFM69_IRQ1_SYNCADDRESS   0x01
#define RFM69_IRQ2_FIFOOVERRUN   0x10	// write 1: clears the FIFO
#define RFM69_IRQ2_PAYLOADREADY  0x04

// RegDioMapping1: DIO0 in bits 7-6. 01 in RX = PayloadReady
#define RFM69_DIO0_PAYLOADREADY  0x40
// RegDioMapping2: ClkOut off
#define RFM69_CLKOUT_OFF         0x07

#define RFM69_FSTEP_HZ           61.03515625f	// FXOSC / 2^19

#endif /* RFM69REGS_H_ */
