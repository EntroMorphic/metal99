/*
 * SH8601 AMOLED controller over QSPI. Pure ISO C99.
 *
 * Wire framing (from the panel's QSPI mode, not the SPI command phase):
 *   command word = { opcode, 0x00, cmd, 0x00 }  sent on ONE line
 *     opcode 0x02 -> parameter write, params follow on one line
 *     opcode 0x32 -> pixel write   , pixels follow on FOUR lines (cmd 0x2C)
 * CS must stay asserted from the command word through the last data byte.
 * Every function here owns that, so no caller ever composes CS by hand.
 */
#ifndef SH8601_PANEL_H
#define SH8601_PANEL_H

#include <stdint.h>

#define SH8601_WIDTH  368
#define SH8601_HEIGHT 448

/* Send a command with optional parameters. params may be NULL when n == 0. */
int sh8601_cmd(uint8_t cmd, const uint8_t *params, uint32_t n);

/* 0x51 - display brightness, 0..255. */
int sh8601_brightness(uint8_t level);

#endif /* SH8601_PANEL_H */
