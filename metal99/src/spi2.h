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

/*
 * TRANSMIT LEDGER - self-checking without a human looking at the screen.
 *
 * The panel cannot be read back, so correctness has depended on someone
 * describing the display. That loop is slow, and it cannot distinguish a wedged
 * panel showing stale content from wrong pixels being drawn - which cost this
 * project several wrong conclusions.
 *
 * What CAN be verified on-device is what the hardware was actually told to
 * send. The ledger counts bytes handed to the peripheral and, when armed,
 * accumulates an order-sensitive digest of them. A caller that knows what it
 * intended can then assert it - catching truncation, duplication and
 * reordering, which is precisely the failure class the banded DMA path
 * exhibited.
 *
 * COUNTING is O(1) per transfer and always on.
 *
 * DIGESTING is O(1) per BYTE and off by default. This header used to claim the
 * whole ledger was O(1) per transfer; it was not. vec_fold() ran over every
 * pixel byte of every transfer in steady state - roughly 184 words per row,
 * ~0.8 ms on a 104-row update - so a verification instrument nothing read
 * outside the self-test was charging the render loop about 12% of its budget
 * forever. Arm it for the self-test, leave it off to ship.
 */
void     spi2_ledger_digest_enable(int on);

/* Reset also clears the content digest and its position weight. It does NOT
 * change whether the digest is armed. */
void     spi2_ledger_reset(void);
uint32_t spi2_ledger_bytes(void);

/* Pixel payload only - command preamble is counted by spi2_ledger_bytes() but
 * excluded here, so a caller can compare against a reference computed from
 * pixel rows alone without knowing the preamble's size. */
uint32_t spi2_ledger_pixel_bytes(void);
uint32_t spi2_ledger_digest(void);

/* Clock-gate, reset, route pins through the GPIO matrix, configure mode 0. */
void spi2_init(void);

/*
 * Drain the output AFIFO. Call with CS IDLE, before starting a new stream.
 *
 * There is a small asynchronous FIFO between SPI_W0..W15 and the wire, and it
 * does not necessarily come up empty for the next transaction. spi2_dma_start()
 * has always reset it - IDF's spi_hal_hw_prepare_tx() does the same - but the
 * FIFO path, the one that actually ships, never did.
 *
 * The symptom is leakage across a span boundary: the first pixels of a span
 * carry bytes belonging to the previous one. On a static three-band test that
 * showed as orange bleeding into the top rows of the next band, and a stray
 * fragment of one band's colour appearing inside another. In motion it reads
 * as debris, and it gets worse the more spans a frame contains - which is why
 * it tracked sub-width marking so closely: elide coalesces rows only when their
 * extents match, so mixed extents split one span into several, and every extra
 * boundary is another chance to leak.
 */
void spi2_flush_afifo(void);

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
