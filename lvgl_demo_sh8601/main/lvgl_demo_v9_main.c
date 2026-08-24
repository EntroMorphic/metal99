#include "bsp/esp-bsp.h"
#include <inttypes.h>
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lv_demos.h"
#include "lvgl.h"

static const char *TAG = "probe";

/* Incremented from an LVGL timer: proves the LVGL task is actually running. */
static volatile uint32_t lv_ticks = 0;
static void heartbeat_cb(lv_timer_t *t) { (void)t; lv_ticks++; }

void app_main(void)
{
    ESP_LOGI(TAG, "app_main entered");

#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(6, 0, 0)
    esp_log_level_t i2c_log_level = esp_log_level_get("i2c.master");
    esp_log_level_set("i2c.master", ESP_LOG_NONE);
#endif
    ESP_LOGI(TAG, "calling bsp_display_start()...");
    lv_display_t *display = bsp_display_start();
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(6, 0, 0)
    esp_log_level_set("i2c.master", i2c_log_level);
#endif
    if (display == NULL) { ESP_LOGE(TAG, "PROBE FAIL: display start returned NULL"); return; }
    ESP_LOGI(TAG, "PROBE OK: display=%p  res=%" PRId32 "x%" PRId32,
             display,
             lv_display_get_horizontal_resolution(display),
             lv_display_get_vertical_resolution(display));

    lv_indev_t *indev = lv_indev_get_next(NULL);
    ESP_LOGI(TAG, "PROBE: indev(touch)=%p type=%d", indev,
             indev ? (int)lv_indev_get_type(indev) : -1);

    ESP_ERROR_CHECK(bsp_display_brightness_set(85));
    ESP_LOGI(TAG, "PROBE OK: brightness set to 85");

    if (bsp_display_lock(0)) {
        lv_demo_widgets();
        lv_timer_create(heartbeat_cb, 100, NULL);
        bsp_display_unlock();
        ESP_LOGI(TAG, "PROBE OK: lv_demo_widgets() created, heartbeat timer armed");
    } else {
        ESP_LOGE(TAG, "PROBE FAIL: could not lock LVGL");
        return;
    }

    uint32_t last = 0;
    for (int i = 0; i < 12; i++) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        uint32_t now = lv_ticks;
        ESP_LOGI(TAG, "t=%2ds  lv_timer_ticks=%" PRIu32 " (+%" PRIu32 "/2s)  heap=%u  psram=%u",
                 (i + 1) * 2, now, now - last,
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        last = now;
    }
    ESP_LOGI(TAG, "PROBE COMPLETE");
    while (true) { vTaskDelay(pdMS_TO_TICKS(1000)); }
}
