/*
 * SPI2 (FSPI) in QSPI mode, driven from registers. Pure ISO C99.
 *
 * Transfer model follows what the SH8601 actually needs, which is NOT the SPI
 * peripheral's command phase: the panel's 32-bit "command" is sent as ordinary
 * 1-line MOSI data with CS held asserted, then pixel bytes follow in a second
 * 4-line (QIO) transaction under the same CS. Sidesteps the hardware command
 * width limit entirely.
 */
#ifndef SPI2_H
#define SPI2_H

#include <stdint.h>

#define SPI2_FIFO_BYTES 64   /* SPI_W0..W15 */

/* Clock-gate, reset, route pins through the GPIO matrix, configure mode 0. */
void spi2_init(void);

/*
 * One FIFO transaction. len must be 1..SPI2_FIFO_BYTES.
 *   quad    : 0 = data on 1 line, 1 = data on 4 lines (QIO)
 *   keep_cs : 1 = leave CS asserted after this transaction
 */
void spi2_xfer(const uint8_t *data, uint32_t len, int quad, int keep_cs);

/* Read back a register - used to prove the peripheral is clocked and alive. */
uint32_t spi2_probe(void);

/* Busy-wait. Deliberately over-estimates CPU frequency so waits are never
 * shorter than requested (the SH8601 sleep-out delay is a minimum). */
void delay_ms(uint32_t ms);

#endif /* SPI2_H */

/* Test hook: override SPI_CLOCK directly so divider configs can be swept. */
void spi2_set_clock_reg(uint32_t v);
uint32_t spi2_get_clock_reg(void);
