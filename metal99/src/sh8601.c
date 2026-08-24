#include "sh8601.h"
#include "spi2.h"
#include <stddef.h>

#define OPCODE_PARAM 0x02u

int sh8601_cmd(uint8_t cmd, const uint8_t *params, uint32_t n)
{
    uint8_t word[4];
    int rc;

    word[0] = OPCODE_PARAM;
    word[1] = 0x00u;
    word[2] = cmd;
    word[3] = 0x00u;

    /* Command word on one line. Hold CS if parameters follow, because the
     * panel treats a CS rise as end-of-command. */
    rc = spi2_xfer(word, 4u, 0, (n > 0u) ? 1 : 0);
    if (rc != SPI2_OK) return rc;

    if (n > 0u) {
        if (params == NULL) return SPI2_E_NULL;
        /* Parameters are one-line for opcode 0x02. spi2_write releases CS on
         * its final chunk, which ends the command. */
        rc = spi2_write(params, n, 0);
        if (rc != SPI2_OK) return rc;
    }
    return SPI2_OK;
}

int sh8601_brightness(uint8_t level)
{
    return sh8601_cmd(0x51u, &level, 1u);
}
