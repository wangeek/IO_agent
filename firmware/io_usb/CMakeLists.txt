idf_component_register(
    SRCS "io_usb.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_tinyusb tinyusb io_disk io_hid esp_hw_support
)
