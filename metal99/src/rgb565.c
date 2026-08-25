/*
 * Colour packing. Pure ISO C99, no hardware.
 *
 * Split out of sh8601.c so the host can compile the same function the device
 * runs. It was previously reachable only by linking the whole panel driver, so
 * tests/host/host_render.c carried its own copy - and a colour helper
 * duplicated between the thing under test and the thing testing it is exactly
 * the drift that put "q4-q7 UNUSED" in two files after q4-q7 were taken.
 */
#include "sh8601.h"

uint16_t sh8601_rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    uint16_t c = (uint16_t)(((r & 0xF8u) << 8) | ((g & 0xFCu) << 3) | (b >> 3));
    return (uint16_t)((c >> 8) | (c << 8));   /* panel wants big-endian */
}
