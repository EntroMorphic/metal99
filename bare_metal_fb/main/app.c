/*
 * Pure ISO C99 application. -std=c99 -pedantic-errors -Wall -Wextra.
 * No ESP-IDF headers. No FreeRTOS: synchronisation is a volatile flag set
 * from the DMA-completion callback, so there is no task, queue or semaphore.
 */
#include "platform.h"
#include "render.h"
#include <string.h>

#define W          368
#define H          448
#define FB_BYTES   (W * H * 2)
#define BAND_H_MAX 64

static volatile int s_done;
static uint16_t    *s_band;
static int          s_band_h = 32;

/* Runs in the SPI ISR. Kept out of flash so a cache-disable window is safe. */
static bool on_trans_done(lcd_io_h io, void *edata, void *ctx)
    __attribute__((section(".iram1.bare")));
static bool on_trans_done(lcd_io_h io, void *edata, void *ctx)
{
    (void)io; (void)edata; (void)ctx;
    s_done = 1;
    return false;
}

/* esp_rom_printf has no %f, so print fixed-point from integer microseconds. */
static void print_us(const char *label, int64_t us)
{
    esp_rom_printf("%s=%d.%02d ms  ", label, (int)(us / 1000), (int)((us % 1000) / 10));
}

static int64_t flush_bands(lcd_panel_h panel, const uint16_t *fb)
{
    int64_t t0 = esp_timer_get_time();
    int y;
    for (y = 0; y < H; y += s_band_h) {
        int bh = (y + s_band_h > H) ? (H - y) : s_band_h;
        memcpy(s_band, fb + (size_t)y * W, (size_t)bh * W * 2);
        s_done = 0;
        if (esp_lcd_panel_draw_bitmap(panel, 0, y, W, y + bh, s_band) != PLAT_OK) {
            esp_rom_printf("draw_bitmap failed at y=%d\n", y);
            return -1;
        }
        {
            int64_t tw = esp_timer_get_time();
            while (!s_done) {
                if (esp_timer_get_time() - tw > 1000000) {
                    esp_rom_printf("flush timeout at y=%d\n", y);
                    return -1;
                }
            }
        }
    }
    return esp_timer_get_time() - t0;
}

typedef void (*raster_fn)(uint16_t *, int, int, uint32_t);

static void run_bench(lcd_panel_h panel, uint16_t *fb[2], const char *name,
                      raster_fn fn, int frames)
{
    int64_t r_tot = 0, f_tot = 0;
    int i;
    for (i = 0; i < frames; i++) {
        uint16_t *cur = fb[i & 1];
        int64_t t0 = esp_timer_get_time();
        fn(cur, W, H, (uint32_t)i * 6);
        r_tot += esp_timer_get_time() - t0;
        {
            int64_t f = flush_bands(panel, cur);
            if (f < 0) { esp_rom_printf("%s aborted\n", name); return; }
            f_tot += f;
        }
    }
    {
        int64_t r = r_tot / frames, f = f_tot / frames, t = r + f;
        int fps10 = (int)(10000000 / (t > 0 ? t : 1));
        esp_rom_printf("%-7s band=%2d  ", name, s_band_h);
        print_us("render", r); print_us("flush", f); print_us("total", t);
        esp_rom_printf("%d.%d fps\n", fps10 / 10, fps10 % 10);
    }
}

void app_main(void);
void app_main(void)
{
    lcd_panel_h panel = NULL;
    lcd_io_h    io    = NULL;
    bsp_display_cfg cfg;
    lcd_io_callbacks cbs;
    uint16_t *fb[2];
    int i, bh;
    uint32_t t;

    esp_rom_printf("\n=== pure C99 bare-metal: %dx%d RGB565, %d B/frame ===\n",
                   W, H, FB_BYTES);
    render_tables_init();

    cfg.max_transfer_sz = FB_BYTES;
    if (bsp_display_new(&cfg, &panel, &io) != PLAT_OK) {
        esp_rom_printf("FAIL: bsp_display_new\n"); return;
    }
    cbs.on_color_trans_done = on_trans_done;
    if (esp_lcd_panel_io_register_event_callbacks(io, &cbs, NULL) != PLAT_OK) {
        esp_rom_printf("FAIL: register callbacks\n"); return;
    }
    bsp_display_brightness_init();
    bsp_display_brightness_set(90);

    for (i = 0; i < 2; i++) {
        fb[i] = (uint16_t *)heap_caps_aligned_alloc(64, FB_BYTES, CAP_SPIRAM);
        if (fb[i] == NULL) { esp_rom_printf("FAIL: fb%d alloc\n", i); return; }
        memset(fb[i], 0, FB_BYTES);
    }
    s_band = (uint16_t *)heap_caps_aligned_alloc(64, (size_t)BAND_H_MAX * W * 2,
                                                 CAP_DMA | CAP_INTERNAL);
    if (s_band == NULL) { esp_rom_printf("FAIL: band alloc\n"); return; }
    esp_rom_printf("fb0=%p fb1=%p band=%p  psram_free=%u\n",
                   (void *)fb[0], (void *)fb[1], (void *)s_band,
                   (unsigned)heap_caps_get_free_size(CAP_SPIRAM));

    render_colorbars(fb[0], W, H);
    flush_bands(panel, fb[0]);
    esp_rom_delay_us(3000000u);

    esp_rom_printf("--- band sweep ---\n");
    for (bh = 8; bh <= BAND_H_MAX; bh <<= 1) {
        s_band_h = bh;
        run_bench(panel, fb, "solid", render_solid, 20);
    }
    s_band_h = 32;

    esp_rom_printf("--- renderers ---\n");
    run_bench(panel, fb, "solid",  render_solid,  30);
    run_bench(panel, fb, "plasma", render_plasma, 30);
    run_bench(panel, fb, "field",  render_field,  30);

    esp_rom_printf("--- live plasma (no FreeRTOS, no IDF headers) ---\n");
    t = 0;
    {
        int64_t last = esp_timer_get_time();
        uint32_t frames = 0;
        for (;;) {
            uint16_t *cur = fb[t & 1u];
            render_plasma(cur, W, H, t * 3u);
            if (flush_bands(panel, cur) < 0) break;
            t++; frames++;
            {
                int64_t now = esp_timer_get_time();
                if (now - last >= 5000000) {
                    int fps10 = (int)((int64_t)frames * 10000000 / (now - last));
                    esp_rom_printf("live: %d.%d fps\n", fps10 / 10, fps10 % 10);
                    frames = 0; last = now;
                }
            }
        }
    }
}
