#include <stddef.h>
#include "io.h"
#include "spi2.h"
#include "sh8601.h"

/*
 * FIRST CONTACT TEST.
 *
 * The panel is already initialised and displaying the previous firmware's last
 * frame - it keeps power and state across CPU resets, and there is no reset
 * pin. So we do NOT run the init sequence yet: a known-good display is the best
 * diagnostic we have and disturbing it would waste it.
 *
 * Instead, breathe the brightness. 0x51 needs zero pixel data, so a visible
 * pulse proves command framing, CS hold, pin routing and quad-vs-single all at
 * once. If the image pulses, we own the panel.
 */
void app_entry(void)
{
    int rc, i;

    con_puts("\r\n=== metal99 : SH8601 first contact ===\r\n");
    con_puts("panel should already show the previous frame.\r\n");
    con_puts("watch for it to PULSE dim/bright.\r\n");

    spi2_init();

    for (i = 0; ; i++) {
        uint8_t level = ((i & 1) == 0) ? 0x00u : 0xFFu;

        rc = sh8601_brightness(level);

        con_puts(((i & 1) == 0) ? "  0x51 <- 0x00 (dim)   rc=" : "  0x51 <- 0xFF (bright) rc=");
        con_dec((int32_t)rc);
        con_puts(rc == SPI2_OK ? "  sent\r\n" : "  SEND FAILED\r\n");

        delay_ms(1500u);
    }
}
