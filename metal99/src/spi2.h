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

/* Return codes. Negative on failure - callers MUST check. */
#define SPI2_OK        0
#define SPI2_E_LEN   (-1)    /* len 0, or > SPI2_FIFO_BYTES */
#define SPI2_E_NULL  (-2)

/* Clock-gate, reset, route pins through the GPIO matrix, configure mode 0. */
void spi2_init(void);

/*
 * One FIFO transaction. len must be 1..SPI2_FIFO_BYTES.
 *   quad    : 0 = data on 1 line, 1 = data on 4 lines (QIO)
 *   keep_cs : 1 = leave CS asserted after this transaction
 *
 * CONTRACT: if keep_cs is 1, CS stays LOW until some later transaction runs
 * with keep_cs 0. Failing to do that leaves the panel selected indefinitely.
 * Prefer spi2_write() below, which cannot get this wrong.
 */
int spi2_xfer(const uint8_t *data, uint32_t len, int quad, int keep_cs);

/*
 * Arbitrary-length write. Chunks internally, holding CS asserted across every
 * chunk and releasing it on the last one. This is the safe entry point: it
 * removes caller-side chunking, which is the easiest way to silently drop
 * pixels and get a partially-blank panel.
 */
int spi2_write(const uint8_t *data, uint32_t len, int quad);

#endif /* SPI2_H */
