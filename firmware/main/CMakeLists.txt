idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES io_disk io_usb io_hid io_payload io_launcher io_cfg io_net io_log io_ota io_led io_lcd io_mcp
)
