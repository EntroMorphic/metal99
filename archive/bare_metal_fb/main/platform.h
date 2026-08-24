/*
 * Hand-written declarations of the external ABI this firmware links against.
 *
 * Deliberately includes NO ESP-IDF header: IDF headers use C11 (_Static_assert)
 * and bare `asm`, neither of which is legal ISO C99. Linking does not require
 * their headers - only matching signatures, which are declared here and pinned
 * to ESP-IDF v5.5.5 / BSP 2.0.0.
 */
#ifndef PLATFORM_H
#define PLATFORM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Opaque handles. IDF hands us pointers; we never dereference them. */
typedef void *lcd_panel_h;
typedef void *lcd_io_h;

#define PLAT_OK 0

/* --- heap (esp_heap_caps.h) --- */
#define CAP_DMA      (1u << 3)
#define CAP_SPIRAM   (1u << 10)
#define CAP_INTERNAL (1u << 11)
void  *heap_caps_aligned_alloc(size_t alignment, size_t size, uint32_t caps);
size_t heap_caps_get_free_size(uint32_t caps);

/* --- time / console (esp_timer.h, esp_rom_sys.h) --- */
int64_t esp_timer_get_time(void);
void    esp_rom_delay_us(uint32_t us);
int     esp_rom_printf(const char *fmt, ...);

/* --- lcd (esp_lcd_panel_ops.h, esp_lcd_types.h) --- */
int esp_lcd_panel_draw_bitmap(lcd_panel_h panel, int x0, int y0,
                              int x1, int y1, const void *px);

typedef bool (*lcd_trans_done_cb)(lcd_io_h io, void *edata, void *ctx);
typedef struct {
    lcd_trans_done_cb on_color_trans_done;
} lcd_io_callbacks;
int esp_lcd_panel_io_register_event_callbacks(lcd_io_h io,
                                              const lcd_io_callbacks *cbs,
                                              void *ctx);

/* --- board support (bsp/display.h) --- */
typedef struct { int max_transfer_sz; } bsp_display_cfg;
int bsp_display_new(const bsp_display_cfg *cfg, lcd_panel_h *panel, lcd_io_h *io);
int bsp_display_brightness_init(void);
int bsp_display_brightness_set(int percent);

/* ISO C99 static assertions (no _Static_assert - that is C11). */
typedef char plat_chk_ptr[sizeof(void *) == 4 ? 1 : -1];
typedef char plat_chk_cbs[sizeof(lcd_io_callbacks) == sizeof(void *) ? 1 : -1];
typedef char plat_chk_cfg[sizeof(bsp_display_cfg) == sizeof(int) ? 1 : -1];

#endif /* PLATFORM_H */
