idf_component_register(
    SRCS "io_ota.c"
    INCLUDE_DIRS "include"
    REQUIRES app_update esp_http_client esp_app_format heap
)
