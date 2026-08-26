/*
 * I2C0 master, driven from registers. Pure ISO C99.
 *
 * Exists for the FT3168 touch controller, which is the only I2C device this
 * project talks to. Written the same way spi2.c was - from the TRM register
 * layout, looked up with tools/reg.sh rather than remembered.
 *
 * WHY A COMMAND LIST, NOT BIT-BANGING. The ESP32-S3 I2C peripheral executes a
 * short program: each I2C_COMD register holds an opcode, a byte count and ACK
 * handling, and TRANS_START runs the sequence. A register read is therefore
 * one descriptor list - START, write addr+reg, repeated START, write addr|R,
 * read n-1 with ACK, read 1 with NACK, STOP - submitted once and polled for
 * completion. No per-bit CPU involvement, which matters because the render
 * loop cannot afford to be blocked (DESIGN.md 3.0).
 *
 * NOT ON THE RENDER PATH. Touch is polled between frames, so the byte-at-a-time
 * handling here is control flow, not bulk data work, and the no-scalar rule
 * (6.9) does not reach it - the same exemption fold.c has.
 */
#ifndef I2C_H
#define I2C_H

#include <stdint.h>

#define I2C_OK          0
#define I2C_E_BUSY    (-1)   /* bus never went idle before we started      */
#define I2C_E_TIMEOUT (-2)   /* TRANS_COMPLETE never arrived               */
#define I2C_E_NACK    (-3)   /* device did not acknowledge - absent?       */
#define I2C_E_ARB     (-4)   /* arbitration lost - another master, or SDA
                              * held low by a device mid-transaction       */
#define I2C_E_LEN     (-5)   /* length outside what one command list holds */

/* Clock-gate, reset, route SDA/SCL through the GPIO matrix as open-drain with
 * pull-ups, and set 400 kHz timing from the 40 MHz XTAL. */
void i2c_init(void);

/* Raw INT_RAW and SR from the last transaction. Diagnostics, kept because they
 * are what located a missing SCLK_ACTIVE bit that made the whole bus look
 * empty - a symptom indistinguishable from "nothing is plugged in". */
/*
 * BRING-UP INSTRUMENTATION - currently called by nothing.
 *
 * These read counters and latched register state that were load-bearing while
 * the four I2C register bugs in DESIGN.md 11.2 was being brought up, and are unreferenced today, so --gc-sections
 * drops them and they cost the image nothing.
 *
 * KEPT, not archived: they are instruments, and instruments belong in the repo
 * (CONTRIBUTING.md). The only debt was that they sat in the working API with
 * no sign saying so - a reader could reasonably take them for part of the
 * interface rather than a debugger's toolkit.
 */
uint32_t i2c_dbg_lines_before(void);  /* bit0 SCL, bit1 SDA; 3 = idle */
uint32_t i2c_dbg_lines_after(void);
uint32_t i2c_dbg_pulses(void);
uint32_t i2c_dbg_int(void);
uint32_t i2c_dbg_sr(void);

/* Which addresses ACK, into `found`. A diagnostic: it separates "the bus is
 * dead" from "something answered, but not where expected". Returns the count. */
uint32_t i2c_scan(uint8_t *found, uint32_t max);

/* Address-only transaction. Returns I2C_OK if the device ACKed its address,
 * I2C_E_NACK if nothing answered. The cheapest possible "is it there?". */
int i2c_probe(uint8_t addr);

/*
 * Read `n` bytes from `reg` of `addr`. n must be 1..I2C_MAX_READ.
 *
 * Bounded by the 32-byte hardware FIFO: this driver deliberately does not
 * implement the multi-segment continuation an arbitrary-length read would need,
 * because the largest thing it reads is a 6-byte touch report. A caller asking
 * for more gets I2C_E_LEN rather than a silently truncated answer.
 */
#define I2C_MAX_READ 30
int i2c_read(uint8_t addr, uint8_t reg, uint8_t *buf, uint32_t n);

/* Write one register. Same FIFO bound. */
int i2c_write1(uint8_t addr, uint8_t reg, uint8_t val);

#endif /* I2C_H */
