/*
 * GDMA channel 0, TX, feeding SPI2. Pure ISO C99.
 *
 * Replaces the 64-byte FIFO path. Telemetry showed 80% of flush time was
 * per-transaction setup across 5,152 transactions - GDMA removes that setup
 * entirely and, being asynchronous, frees the CPU to render the next band
 * while the current one is on the wire.
 *
 * Descriptor size/length fields are 12 bits, so ONE descriptor carries at most
 * 4095 bytes. A 368px row is 736 B, so a descriptor holds at most 5 rows.
 * Bands are therefore described by a CHAIN, not a single descriptor.
 */
#ifndef GDMA_H
#define GDMA_H

#include <stdint.h>

#define GDMA_MAX_XFER   4095u          /* 12-bit size field       */
#define GDMA_OK          0
#define GDMA_E_HANG    (-1)

/* Hardware descriptor. Layout fixed by silicon - do not reorder. */
typedef struct gdma_desc {
    uint32_t          dw0;             /* size:12 len:12 rsvd:4 err_eof:1 rsvd:1 suc_eof:1 owner:1 */
    const void       *buffer;
    struct gdma_desc *next;
} __attribute__((aligned(4))) gdma_desc;

#define GDMA_DW0(size, len, eof) \
    (((uint32_t)(size) & 0xFFFu) | (((uint32_t)(len) & 0xFFFu) << 12) | \
     ((uint32_t)((eof) ? 1u : 0u) << 30) | (1u << 31))   /* owner = DMA */

/* Clock-gate, reset, bind channel 0 TX to SPI2. Call after spi2_init(). */
void gdma_init(void);

/* Kick off a descriptor chain. Returns immediately - the CPU is free. */
void gdma_start(const gdma_desc *first);

/* Block until the chain completes. Bounded; never spins forever. */
int  gdma_wait(void);

#endif /* GDMA_H */
