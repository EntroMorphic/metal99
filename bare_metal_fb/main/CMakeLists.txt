idf_component_register(SRCS "app.c" "render_c99.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_timer esp_lcd driver esp_psram esp_rom heap)

# EVERY source file in this component is strict ISO C99.
set_source_files_properties(app.c render_c99.c PROPERTIES
                            COMPILE_OPTIONS "-std=c99;-pedantic-errors;-Wall;-Wextra")
