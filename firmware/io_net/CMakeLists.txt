idf_component_register(
    SRCS "io_net.c"
    INCLUDE_DIRS "include"
    REQUIRES io_cfg esp_wifi esp_netif esp_event lwip
)

# ---------------------------------------------------------------------------
# lwIP 自带的 mDNS responder
#
# IDF 5.5 默认**不编译** lwIP 的 mdns app(只有 sntp / netbiosns),而
# esp-mdns 又在组件管理器里 —— 与本项目"关组件管理器 + 离线可复现"的约束冲突。
#
# 但 lwIP 的 responder 本来就够用:mdns_resp_add_service() 支持 DNS-SD 服务
# 记录 + 端口 + TXT,正好是 MCP 客户端查 _ioagent._tcp.local 需要的东西。
# 所以直接把 IDF 树里这三个文件编进本组件(不 vendored 副本,始终跟 IDF 版本一致)。
# ---------------------------------------------------------------------------
target_sources(${COMPONENT_LIB} PRIVATE
    "${IDF_PATH}/components/lwip/lwip/src/apps/mdns/mdns.c"
    "${IDF_PATH}/components/lwip/lwip/src/apps/mdns/mdns_domain.c"
    "${IDF_PATH}/components/lwip/lwip/src/apps/mdns/mdns_out.c"
)
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    LWIP_MDNS_RESPONDER=1
    MDNS_MAX_SERVICES=2
)
