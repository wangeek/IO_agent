idf_component_register(
    SRCS "io_cfg.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash esp_wifi driver io_disk io_led io_log
)
