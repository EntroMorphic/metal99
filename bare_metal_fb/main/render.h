/* Pure ISO C99 rendering interface. No ESP-IDF, no FreeRTOS, no platform
 * headers - only <stdint.h>/<stddef.h>. Compiles with -std=c99 -pedantic,
 * and builds unmodified on a host for testing. */
#ifndef RENDER_H
#define RENDER_H

#include <stddef.h>
#include <stdint.h>

void     render_tables_init(void);
uint16_t render_rgb565(uint8_t r, uint8_t g, uint8_t b);

/* All rasterizers take an explicit surface: no globals, no fixed geometry. */
void render_solid(uint16_t *fb, int w, int h, uint32_t t);
void render_plasma(uint16_t *fb, int w, int h, uint32_t t);
void render_field(uint16_t *fb, int w, int h, uint32_t t);
void render_colorbars(uint16_t *fb, int w, int h);

#endif /* RENDER_H */
