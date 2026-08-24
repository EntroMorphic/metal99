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
/* Distinct codes per stage. A single E_HANG told us a transfer failed but not
 * WHERE, and three rounds were spent fixing the wrong stage. */
#define SPI2_E_SYNC  (-5)    /* SPI_UPDATE never cleared      */
#define SPI2_E_USR   (-6)    /* SPI_CMD.USR never cleared     */
#define SPI2_E_DMA   (-7)    /* GDMA never signalled done     */

/*
 * Set the SPI bus clock. Legitimate configuration, not probing: it changes how
 * REAL frames are sent, and correctness is judged by the panel rendering them.
 *
 *   40 -> XTAL  /1   (always available)
 *   80 -> APB   /1   (requires the PLL; APB is 20 MHz without it)
 *
 * 80 MHz is above the vendor BSP's choice of 40. Whether the SH8601 accepts it
 * is the open question - a panel that cannot keep up shows corruption, which
 * is visible immediately.
 */
int spi2_set_clock(uint32_t mhz);

/*
 * Force CS high, transmitting nothing.
 *
 * Needed because CS_KEEP_ACTIVE holds CS asserted until a transaction runs
 * WITHOUT it - so an error return in the middle of a held-CS stream leaves the
 * panel selected indefinitely, and the next command word is then swallowed as
 * pixel data. This takes the pin to plain GPIO, drives it high, and hands it
 * back to SPI2, so no bytes reach the panel.
 */
void spi2_cs_release(void);

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

/*
 * Split async form of the above. The CPU is otherwise idle for the whole
 * transfer, so a caller can render the NEXT band between start and finish.
 *
 * Every _start MUST be matched by a _finish before the descriptors or their
 * buffers are touched again.
 */
int spi2_dma_start(const struct gdma_desc *chain, uint32_t len, int quad, int keep_cs);
int spi2_dma_finish(void);

#endif /* SPI2_H */
