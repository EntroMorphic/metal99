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
#define GDMA_E_ADDR    (-2)    /* descriptor outside internal SRAM */
#define GDMA_E_LEN     (-3)    /* transfer exceeds the 12-bit size field */

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

/* Block until the chain completes. Bounded; never spins forever.
 * MUST be called before the descriptor or its buffer is reused. */
int  gdma_wait(void);

/* Descriptors are fetched via a 20-bit address field, so they must live in
 * internal SRAM. Checked rather than assumed. */
int  gdma_desc_addr_ok(const void *p);

/* Reset the channel and re-arm a chain. Recovery for a transfer that never
 * started - the engine can swallow the first START after init. */
int  gdma_restart(const gdma_desc *first);

/* Raw interrupt status last observed by gdma_wait() - for diagnosis. */
uint32_t gdma_last_status(void);

/* Register snapshot, for diagnosing a transfer that signalled nothing. */
/*
 * BRING-UP INSTRUMENTATION - currently called by nothing.
 *
 * These read counters and latched register state that were load-bearing while
 * the banded DMA path was being brought up, and are unreferenced today, so --gc-sections
 * drops them and they cost the image nothing.
 *
 * KEPT, not archived: they are instruments, and instruments belong in the repo
 * (CONTRIBUTING.md). The only debt was that they sat in the working API with
 * no sign saying so - a reader could reasonably take them for part of the
 * interface rather than a debugger's toolkit.
 */
uint32_t gdma_dbg_link(void);
uint32_t gdma_dbg_conf0(void);

#endif /* GDMA_H */
