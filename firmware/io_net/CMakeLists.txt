idf_component_register(
    SRCS "io_net.c"
    INCLUDE_DIRS "include"
    REQUIRES io_cfg esp_wifi esp_netif esp_event lwip
)
