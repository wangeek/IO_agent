idf_component_register(
    SRCS "io_led.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_rmt io_launcher io_net
)
