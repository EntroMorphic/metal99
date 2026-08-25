#include <stddef.h>
#include "touch.h"
#include "i2c.h"
#include "io.h"
#include "sh8601.h"

#define FT_ADDR      0x38u

/*
 * FT5x06 register protocol, as used by the FT3168.
 *
 * 0x02 holds the number of contacts in its low nibble. Each contact is six
 * bytes from 0x03: XH XL YH YL weight misc, where XH's top two bits are the
 * event flag and YH's top nibble is the tracking id. Reading status and both
 * contacts is therefore one 13-byte transaction rather than five.
 */
#define FT_REG_STATUS 0x02u
#define FT_REG_CHIPID 0xA3u
#define FT_REG_VENDOR 0xA8u
#define FT_VENDOR_FOCALTECH 0x11u

#define GPIO_IN_REG   REG32(0x6000403Cu)
#define PIN_TOUCH_INT 21

static uint8_t  g_chip, g_vendor;
static uint32_t g_polls, g_int_low, g_polls_with_touch;
static int      g_last_io;

int touch_dbg_io(void) { return g_last_io; }

uint32_t touch_dbg_polls(void)      { return g_polls; }
uint32_t touch_dbg_int_low(void)    { return g_int_low; }
uint32_t touch_dbg_with_touch(void) { return g_polls_with_touch; }

uint8_t touch_chip_id(void)   { return g_chip; }
uint8_t touch_vendor_id(void) { return g_vendor; }

int touch_init(void)
{
    uint8_t v;
    int try;

    /*
     * RETRY, because one shot is not enough and the failure is intermittent.
     *
     * The first working build identified the part cleanly; the next boot of the
     * same binary NACKed. The controller is not always ready to talk by the
     * time we get here - there is no reset line to sequence it with, so it
     * comes up on its own schedule - and a single attempt turns a device that
     * is merely still waking into one that is reported absent.
     *
     * Ten attempts, 20 ms apart: 200 ms against the ~350 ms sh8601_init that
     * already precedes this, so it costs nothing anyone will notice and removes
     * a class of boot that looks like a hardware fault.
     */
    for (try = 0; try < 10; try++) {
        if (i2c_probe(FT_ADDR) == I2C_OK &&
            i2c_read(FT_ADDR, FT_REG_VENDOR, &v, 1u) == I2C_OK && v != 0x00u) {
            g_vendor = v;
            break;
        }
        delay_ms(20u);
    }
    if (try == 10) return TOUCH_E_ABSENT;

    if (i2c_read(FT_ADDR, FT_REG_CHIPID, &v, 1u) != I2C_OK) return TOUCH_E_IO;
    g_chip = v;

    /* Checked, not assumed. A CST816 on a V2 board would ACK 0x38 and return
     * something else entirely, and reading its registers as if they were
     * FocalTech's would produce plausible-looking coordinates from nowhere. */
    if (g_vendor != FT_VENDOR_FOCALTECH) return TOUCH_E_ID;
    return TOUCH_OK;
}

int touch_poll(touch_state *st)
{
    uint8_t b[1u + 6u * TOUCH_MAX_POINTS];
    int rc, i;
    uint32_t n;

    if (st == NULL) return TOUCH_E_IO;
    st->n = 0u;

    /*
     * INT GATES THE READ, and it is not only an optimisation.
     *
     * The line is active low - the Waveshare BSP declares `.interrupt = 0` and
     * the measured counters agree - and it means "there is a report worth
     * reading". Polling the status register without it returns whatever the
     * controller last held, which is not the same as "no contact": it produced
     * phantom contacts that flickered on and off, so the coordinate label
     * churned between set and cleared EVERY frame. That cost 6,656 px a frame
     * for a screen nobody was touching, and showed up as artifacts around the
     * text rows.
     *
     * The gate was off during bring-up on purpose - polarity was an assumption
     * then, and a wrong one would have made a working controller look dead. It
     * is measured now, so the gate goes back on.
     *
     * It also does what it was always meant to: an idle screen costs a GPIO
     * read rather than a ~150 us I2C transaction.
     */
    g_polls++;
    if ((GPIO_IN_REG & (1u << PIN_TOUCH_INT)) != 0u) return TOUCH_OK;
    g_int_low++;

    rc = i2c_read(FT_ADDR, FT_REG_STATUS, b, sizeof b);
    if (rc != I2C_OK) { g_last_io = rc; return TOUCH_E_IO; }

    n = (uint32_t)(b[0] & 0x0Fu);
    if (n > (uint32_t)TOUCH_MAX_POINTS) n = (uint32_t)TOUCH_MAX_POINTS;

    for (i = 0; i < (int)n; i++) {
        const uint8_t *p = &b[1 + 6 * i];
        uint16_t x = (uint16_t)(((uint16_t)(p[0] & 0x0Fu) << 8) | p[1]);
        uint16_t y = (uint16_t)(((uint16_t)(p[2] & 0x0Fu) << 8) | p[3]);
        /* Clamp rather than reject: a controller reporting slightly outside the
         * active area is normal at the bezel, and a caller drawing at the
         * returned coordinate must not be handed something off-screen. */
        if (x >= SH8601_WIDTH)  x = (uint16_t)(SH8601_WIDTH - 1);
        if (y >= SH8601_HEIGHT) y = (uint16_t)(SH8601_HEIGHT - 1);
        st->p[i].x  = x;
        st->p[i].y  = y;
        st->p[i].id = (uint8_t)(p[2] >> 4);
    }
    st->n = (uint8_t)n;
    if (n > 0u) g_polls_with_touch++;
    return TOUCH_OK;
}
