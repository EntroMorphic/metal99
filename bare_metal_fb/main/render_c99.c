/* Strict ISO C99. Compiled with -std=c99 -pedantic-errors.
 * Note: M_PI is NOT in ISO C99 (it is POSIX), so we define our own. */
#include "render.h"
#include <math.h>

static const float R_PI = 3.14159265358979323846f;

static uint8_t  s_sin[1024];
static uint16_t s_pal[256];

uint16_t render_rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    /* SH8601 expects big-endian RGB565 (BSP sets swap_bytes=true). Done with
     * shifts rather than __builtin_bswap16 so this stays ISO, not GNU. */
    uint16_t c = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    return (uint16_t)((c >> 8) | (c << 8));
}

void render_tables_init(void)
{
    int i;
    for (i = 0; i < 1024; i++) {
        s_sin[i] = (uint8_t)(127.5f * (1.0f + sinf(2.0f * R_PI * (float)i / 1024.0f)));
    }
    for (i = 0; i < 256; i++) {
        float u = (float)i / 255.0f;
        float e = u * u;
        s_pal[i] = render_rgb565((uint8_t)(255.0f * e * e * e),
                                 (uint8_t)(255.0f * e * e),
                                 (uint8_t)(255.0f * e));
    }
}

void render_solid(uint16_t *fb, int w, int h, uint32_t t)
{
    uint16_t c = s_pal[(t * 4) & 0xFF];
    int n = w * h, i;
    for (i = 0; i < n; i++) fb[i] = c;
}

void render_plasma(uint16_t *fb, int w, int h, uint32_t t)
{
    int x, y;
    for (y = 0; y < h; y++) {
        uint8_t sy = s_sin[(y * 3 + (int)t) & 1023];
        uint16_t *row = fb + (size_t)y * (size_t)w;
        for (x = 0; x < w; x++) {
            uint8_t v = (uint8_t)(s_sin[(x * 4 + (int)t) & 1023]
                                + sy
                                + s_sin[((x + y) * 2 + (int)t * 2) & 1023]);
            row[x] = s_pal[v];
        }
    }
}

void render_field(uint16_t *fb, int w, int h, uint32_t t)
{
    float ph  = (float)t * 0.15f;
    float s1x = (float)w * 0.5f + 90.0f * cosf(ph * 0.70f);
    float s1y = (float)h * 0.5f + 90.0f * sinf(ph * 0.50f);
    float s2x = (float)w * 0.5f - 90.0f * cosf(ph * 0.40f);
    float s2y = (float)h * 0.5f - 90.0f * sinf(ph * 0.60f);
    float pw  = ph * 20.0f;
    int x, y;

    for (y = 0; y < h; y++) {
        uint16_t *row = fb + (size_t)y * (size_t)w;
        for (x = 0; x < w; x++) {
            float dx1 = (float)x - s1x, dy1 = (float)y - s1y;
            float dx2 = (float)x - s2x, dy2 = (float)y - s2y;
            float d1 = sqrtf(dx1 * dx1 + dy1 * dy1);
            float d2 = sqrtf(dx2 * dx2 + dy2 * dy2);
            int i1 = ((int)(d1 * 8.0f - pw)) & 1023;
            int i2 = ((int)(d2 * 8.0f - pw)) & 1023;
            row[x] = s_pal[(uint8_t)((s_sin[i1] + s_sin[i2]) >> 1)];
        }
    }
}

void render_colorbars(uint16_t *fb, int w, int h)
{
    uint16_t bars[5];
    int x, y;
    bars[0] = render_rgb565(255, 0, 0);
    bars[1] = render_rgb565(0, 255, 0);
    bars[2] = render_rgb565(0, 0, 255);
    bars[3] = render_rgb565(255, 255, 255);
    bars[4] = render_rgb565(0, 0, 0);
    for (y = 0; y < h; y++) {
        uint16_t *row = fb + (size_t)y * (size_t)w;
        int band = (y * 5) / h;
        for (x = 0; x < w; x++) row[x] = bars[band];
    }
}
