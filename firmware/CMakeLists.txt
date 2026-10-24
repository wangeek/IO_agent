cmake_minimum_required(VERSION 3.16)

# ---------------------------------------------------------------------------
# 组件管理器:关闭
#
# 原因:esp_tinyusb / tinyusb 以 vendored 源码形式放在 components/ 下,不走组件
# 注册表。esp_tinyusb 的 CMakeLists 已经支持"本地 tinyusb 组件"模式:
#   idf_build_get_property(idf_component_manager IDF_COMPONENT_MANAGER)
#   if(NOT idf_component_manager) list(APPEND req tinyusb) endif()
#
# 开关是**环境变量** IDF_COMPONENT_MANAGER(见 IDF tools/cmake/project.cmake:60),
# 不是 CMake 变量,所以这里必须用 set(ENV{...}) 写进程环境。
# 关闭后:idf_component.yml 的依赖被忽略 -> 完全离线、可复现构建。
# 详见 components/README-vendor-patches.md
# ---------------------------------------------------------------------------
set(ENV{IDF_COMPONENT_MANAGER} 0)

# 组件目录一律带 io_ 前缀:避免与 ESP-IDF 内置组件同名冲突
# (IDF v5.5 自带 usb 组件 = USB Host 栈,叫 usb 会把它顶掉)。
#
# M1 只落地磁盘 / USB 复合设备 / HID 三块;io_cfg / io_capability 属于后续。
# io_payload 是 T2 的 flash payload 仓(script0/script1 A/B)。
# io_launcher 是注入链路(拉起目标机 watcher)。
# io_cfg / io_net / io_mcp 是 M5 的远程链路:NVS 配置 + WiFi/mDNS + MCP server。
# 注意:EXTRA_COMPONENT_DIRS 指向不存在的目录会让 CMake 直接 FATAL_ERROR。
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/io_disk"
    "${CMAKE_CURRENT_LIST_DIR}/io_usb"
    "${CMAKE_CURRENT_LIST_DIR}/io_hid"
    "${CMAKE_CURRENT_LIST_DIR}/io_payload"
    "${CMAKE_CURRENT_LIST_DIR}/io_launcher"
    "${CMAKE_CURRENT_LIST_DIR}/io_cfg"
    "${CMAKE_CURRENT_LIST_DIR}/io_net"
    "${CMAKE_CURRENT_LIST_DIR}/io_mcp"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(sophon)
