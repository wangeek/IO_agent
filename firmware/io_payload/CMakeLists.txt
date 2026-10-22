idf_component_register(
    SRCS "io_payload.c"
    INCLUDE_DIRS "include"
    REQUIRES io_disk esp_partition esp_timer
)
