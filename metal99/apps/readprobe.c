/*
 * READPROBE - can the SH8601 be read back?
 *
 * Three headers in this repo state that it cannot, and that sentence has
 * shaped everything: it is why correctness depends on a human describing the
 * screen, why the transmit ledger exists, and why a defect that corrupts bytes
 * DOWNSTREAM of the ledger has outlived every attempt to find it. Every
 * instrument this project owns sits upstream of the fault.
 *
 * Nothing in the repo records the premise being tested. So: sweep the standard
 * MIPI DCS read commands across a range of dummy-cycle counts, print
 * everything, and let the bytes decide.
 *
 * WHAT WOULD COUNT AS AN ANSWER. All-0x00 or all-0xFF is the panel driving
 * nothing and the line sitting at whatever the pull resolves to - that is a
 * NO. A stable, repeatable, non-trivial pattern is a YES, and the one to look
 * for hardest is 0x0A (RDDPM), whose bits are known: bit 2 display-on, bit 4
 * sleep-out, bit 7 booster. We drive the panel out of sleep and into display-on
 * during init, so a working read returns something with those bits SET - and
 * that is a value we can predict in advance rather than rationalise afterwards.
 *
 * The W registers are zeroed before each read (see spi2_read), so a panel that
 * answers nothing cannot hand back the previous transfer and look convincing.
 *
 * APP=readprobe ./metal99/build.sh
 */
#include <stdint.h>
#include "app.h"
#include "io.h"
#include "sh8601.h"
#include "spi2.h"
#include "vec.h"

#define OPCODE_READ 0x03u

/*
 * Round 1 swept commands and dummy cycles on ONE wire - data returning on
 * FSPIQ, which is D1/GPIO5, where a normal half-duplex SPI slave would drive
 * it. Nothing. That is not yet an answer, because it tested one of three ways
 * this panel could reply:
 *
 *   D1, 1-line   what round 1 did. Standard MISO position.
 *   D0, 1-line   true 3-wire half duplex, where the panel drives the same line
 *                the host just used for the command. Common on these AMOLED
 *                parts, and invisible to round 1: we were listening on a wire
 *                nothing was talking on.
 *   D0-D3, quad  a quad read. Needs FREAD_QUAD, which round 1 never set.
 *
 * The GPIO matrix routes a peripheral's INPUT independently of its output, so
 * pointing FSPIQ's input at GPIO4 costs one register write and no rewiring.
 */
#define GPIO_FUNC_IN_SEL(sig) REG32(0x60004154u + 4u * (uint32_t)(sig))
#define GPIO_SIG_IN_SEL       (1u << 7)
#define SIG_FSPIQ             102u
#define SIG_FSPID             103u
#define PIN_D0                4u
#define PIN_D1                5u

#define SPI2_BASE_L   0x60024000u
#define SPI_CTRL_L    REG32(SPI2_BASE_L + 0x08u)
#define CTRL_FREAD_QUAD (1u << 15)

/* Point the peripheral's MISO input at a pad. Output routing is untouched, so
 * the panel is still driven exactly as before. */
static void listen_on(uint32_t gpio)
{
    GPIO_FUNC_IN_SEL(SIG_FSPIQ) = (gpio & 0x3Fu) | GPIO_SIG_IN_SEL;
}

static int g_quad;
static int g_total_hits;

/* Command, then read, under one CS. */
static int panel_read(uint8_t cmd, uint8_t *dst, uint32_t len, uint32_t dummy)
{
    static uint8_t VEC_ALIGN word[16];
    int rc;

    word[0] = OPCODE_READ; word[1] = 0x00u; word[2] = cmd; word[3] = 0x00u;
    rc = spi2_xfer(word, 4u, 0 /* 1-line */, 1 /* hold CS */);
    if (rc != SPI2_OK) { spi2_cs_release(); return rc; }

    if (g_quad) SPI_CTRL_L = SPI_CTRL_L | CTRL_FREAD_QUAD;
    rc = spi2_read(dst, len, dummy);
    if (g_quad) SPI_CTRL_L = SPI_CTRL_L & ~CTRL_FREAD_QUAD;
    if (rc != SPI2_OK) spi2_cs_release();
    return rc;
}

static int interesting(const uint8_t *b, uint32_t n)
{
    uint32_t i;
    for (i = 0u; i < n; i++) if (b[i] != 0x00u && b[i] != 0xFFu) return 1;
    return 0;
}

static void sweep(const char *what, uint32_t listen_gpio, int quad)
{
    static const uint8_t CMDS[] = { 0x04u, 0x09u, 0x0Au, 0x0Cu, 0xDAu, 0xDBu, 0xDCu };
    static const uint32_t DUMMY[] = { 0u, 1u, 2u, 4u, 8u, 16u, 24u, 32u };
    uint8_t buf[8];
    uint32_t d;
    int c, hits = 0;

    listen_on(listen_gpio);
    g_quad = quad;

    con_puts("\r\n-- "); con_puts(what); con_puts(" --\r\n");

    for (c = 0; c < (int)(sizeof CMDS); c++) {
        for (d = 0u; d < (uint32_t)(sizeof DUMMY / sizeof DUMMY[0]); d++) {
            uint32_t i;
            int rc;
            for (i = 0u; i < sizeof buf; i++) buf[i] = 0u;

            rc = panel_read(CMDS[c], buf, 4u, DUMMY[d]);
            if (rc != SPI2_OK) {
                con_puts("0x"); con_hex32(CMDS[c]);
                con_puts("  d="); con_dec((int32_t)DUMMY[d]);
                con_puts("  rc="); con_dec((int32_t)rc); con_puts("\r\n");
                continue;
            }
            if (!interesting(buf, 4u)) continue;   /* all 00 or all FF: no answer */

            hits++;
            con_puts("0x"); con_hex32(CMDS[c]);
            con_puts("  d="); con_dec((int32_t)DUMMY[d]);
            con_puts("  ");
            for (i = 0u; i < 4u; i++) { con_hex32(buf[i]); con_puts(" "); }
            con_puts("<-- ANSWER\r\n");
        }
    }

    con_puts("   "); con_dec((int32_t)hits); con_puts(" non-trivial\r\n");
    g_total_hits += hits;
}

static void rp_init(void)
{
    g_total_hits = 0;
    con_puts("\r\nreadprobe: can this panel be read back at all?\r\n");

    sweep("D1 (FSPIQ), 1-line", PIN_D1, 0);
    sweep("D0 (FSPID), 1-line - true half duplex", PIN_D0, 0);
    sweep("D0, QUAD read", PIN_D0, 1);
    sweep("D1, QUAD read", PIN_D1, 1);

    listen_on(PIN_D1);          /* restore the normal routing */
    con_puts("\r\nreadprobe: "); con_dec((int32_t)g_total_hits);
    con_puts(" total non-trivial responses\r\n");
    con_puts(g_total_hits ? "readprobe: THE PANEL ANSWERS.\r\n"
                          : "readprobe: silent on every wire. Premise holds.\r\n");
}
static int  rp_frame(uint32_t f) { (void)f; return 0; }

const app_t APP = { "readprobe", 1u, rp_init, rp_frame, 0 };
