idf_component_register(
    SRCS "io_lcd.c" "font_glcd5x7.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_driver_spi
)
