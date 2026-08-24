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
#define SPI2_E_ALIGN (-3)    /* data not 16-byte aligned (see contract below) */
#define SPI2_E_HANG  (-4)    /* peripheral did not complete - clock stopped? */

/* Clock-gate, reset, route pins through the GPIO matrix, configure mode 0. */
void spi2_init(void);

/*
 * One FIFO transaction. len must be 1..SPI2_FIFO_BYTES.
 *
 * ALIGNMENT CONTRACT: `data` MUST be 16-byte aligned (use VEC_ALIGN). The FIFO
 * is loaded with vector stores, so a misaligned pointer is rejected with
 * SPI2_E_ALIGN rather than silently falling back to a scalar path.
 *
 * Up to 15 bytes past `len` may be READ from the source and written into FIFO
 * registers beyond the transmitted length. Those bytes are never sent
 * (MS_DLEN bounds the transfer), but the source buffer must be padded to a
 * 16-byte multiple so the over-read stays inside your own allocation.
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

/*
 * DMA transfer. `chain` is a GDMA descriptor list describing the bytes; the
 * FIFO is bypassed entirely, so the 16-byte alignment contract above does not
 * apply (GDMA reads memory directly).
 *
 * Sets DMA_TX_ENA for the duration and clears it afterwards, so command
 * traffic keeps working through the FIFO path.
 */
struct gdma_desc;
int spi2_xfer_dma(const struct gdma_desc *chain, uint32_t len, int quad, int keep_cs);

#endif /* SPI2_H */
