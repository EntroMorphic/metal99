/*
 * STOREPROBE - does a 128-bit vector LOAD see a 16-bit scalar STORE?
 *
 * Every real rowfn builds a row the same way: vec_fill16 lays down the
 * background with EE.VST.128 vector stores, then individual pixels are written
 * with ordinary 16-bit scalar stores (vg.c: `row[x] = s->colour`, and gfx does
 * the equivalent around glyphs). spi2_xfer then reads that buffer straight back
 * with EE.VLD.128 to load the FIFO.
 *
 * VECTOR STORE -> SCALAR STORE -> VECTOR LOAD, on the same bytes, microseconds
 * apart. Four rounds of spanlab never once exercised that: its rowfn was pure
 * scalar or pure vec_fill16, never mixed - which is exactly why it stayed clean
 * while the game did not.
 *
 * If the vector load can miss a scalar store still sitting in a store buffer,
 * the FIFO ships stale bytes: the background where a line should be, or - since
 * sh8601 reuses one row buffer for every row - the PREVIOUS row's pixels. That
 * is debris, it is worse where there are more small scalar writes (thin lines,
 * seven-segment digits, glyphs), and it is invisible to every instrument we
 * own, because the ledger digests the buffer with SCALAR reads and the CPU
 * always sees its own stores.
 *
 * This probe writes the buffer the way a rowfn does, reads it back the way the
 * FIFO does, and compares. It needs no panel and no human.
 *
 * APP=storeprobe ./metal99/build.sh
 */
#include <stdint.h>
#include "app.h"
#include "io.h"
#include "sh8601.h"
#include "vec.h"

#define N_PX     368
#define N_VEC    (N_PX * 2 / VEC_BYTES)      /* 46 */

static uint16_t VEC_ALIGN g_src[N_PX];
static uint16_t VEC_ALIGN g_dst[N_PX];

/*
 * Copy src->dst with the SAME instruction pair spi2_xfer uses to fill the FIFO.
 * q3 is the FIFO register per vec.h's allocation table; borrowing it here is
 * safe because no transfer is in flight while this runs.
 */
static void vector_readback(const uint16_t *src, uint16_t *dst, uint32_t vectors)
{
    const uint8_t *s = (const uint8_t *)src;
    uint8_t *d = (uint8_t *)dst;
    __asm__ __volatile__ (
        "1:                                  \n"
        "  ee.vld.128.ip  " VEC_Q_FIFO ", %1, 16 \n"
        "  ee.vst.128.ip  " VEC_Q_FIFO ", %0, 16 \n"
        "  addi.n         %2, %2, -1         \n"
        "  bnez           %2, 1b             \n"
        : "+a"(d), "+a"(s), "+a"(vectors) : : "memory");
}

/*
 * One trial: background by vector store, then `ink` pixels by scalar store,
 * then read back through the vector unit. `barrier` inserts MEMW between the
 * scalar stores and the vector load - if that alone fixes a mismatch, the
 * defect is store ordering and the fix is one instruction.
 */
static int trial(uint32_t seed, int ink, int barrier)
{
    uint32_t i, r = seed;
    int bad = 0;

    vec_fill16(g_src, 0x0841u, N_VEC);          /* background */
    for (i = 0; i < N_PX; i++) g_dst[i] = 0xDEADu;

    /* Scalar pixel writes, scattered the way thin lines and glyph strokes are. */
    for (i = 0; i < (uint32_t)ink; i++) {
        r = r * 1664525u + 1013904223u;
        g_src[(r >> 16) % N_PX] = 0xF81Fu;
    }

    if (barrier) __asm__ __volatile__ ("memw" ::: "memory");

    vector_readback(g_src, g_dst, N_VEC);

    for (i = 0; i < N_PX; i++) if (g_dst[i] != g_src[i]) bad++;
    return bad;
}

static void run(const char *what, int ink, int barrier)
{
    uint32_t t;
    int total = 0, worst = 0;

    for (t = 0u; t < 4000u; t++) {
        int bad = trial(t * 2654435761u + 1u, ink, barrier);
        total += bad;
        if (bad > worst) worst = bad;
    }
    con_puts("  "); con_puts(what);
    con_puts(": mismatches="); con_dec((int32_t)total);
    con_puts(" worst-row="); con_dec((int32_t)worst);
    con_puts(total ? "   <-- VECTOR LOAD MISSED A SCALAR STORE\r\n" : "   ok\r\n");
}

static void sp_init(void)
{
    con_puts("\r\nstoreprobe: vector store -> scalar store -> vector load\r\n");
    run("2 scalar px, no barrier  ", 2,   0);
    run("2 scalar px, MEMW        ", 2,   1);
    run("40 scalar px, no barrier ", 40,  0);
    run("40 scalar px, MEMW       ", 40,  1);
    run("300 scalar px, no barrier", 300, 0);
    con_puts("storeprobe: done\r\n");
}

static int sp_frame(uint32_t f) { (void)f; return 0; }

const app_t APP = { "storeprobe", 1u, sp_init, sp_frame, 0 };
