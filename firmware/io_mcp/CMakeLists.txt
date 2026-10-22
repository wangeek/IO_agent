idf_component_register(
    SRCS "io_mcp.c"
    INCLUDE_DIRS "include"
    REQUIRES io_cfg io_disk io_hid io_launcher io_net io_payload io_usb esp_http_server esp_timer json
)
