idf_component_register(
    SRCS "io_disk.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_psram
)

# ---------------------------------------------------------------------------
# 下面这段只在真正的构建阶段执行
#
# IDF 的"早展开"阶段会在 CMake 的 script mode(cmake -P)里再跑一遍每个组件的
# CMakeLists.txt,用来收集 REQUIRES / INCLUDE_DIRS。script mode 下
# add_custom_command / target_sources / set_property(DIRECTORY ...) 都不可
# 脚本化,调用即报错并中止整个配置阶段。
#
# 镜像生成 + target_add_binary_data 只在真实构建里有意义(它把镜像转成
# 汇编源文件并挂到组件库上),早展开阶段直接跳过。
# IDF 官方组件是靠 Kconfig 变量在早展开期为假来达到同样效果(如 mbedtls 的
# if(CONFIG_MBEDTLS_CUSTOM_CERTIFICATE_BUNDLE),我们没有那种天然守卫。
# ---------------------------------------------------------------------------
if(NOT CMAKE_SCRIPT_MODE_FILE)

# ---------------------------------------------------------------------------
# 构建期生成 FAT16 镜像 + 槽位表,并内嵌进固件
#
# T2(2026-10-22 起):出厂镜像**不再内嵌 watcher/watch.ps1**。
#
# 理由:watcher 是"AI 现场就能生成"的文件,把它烤进固件是纯浪费 ——
# app 分区实占 161KB 里,有 20KB 是它。现在出厂镜像只做两件事:
#   1. 声明一个 4MB 的 FAT16 卷(元数据区,约 48KB);
#   2. 在卷末尾**预留**若干槽位(簇链 + 目录项进镜像,内容不进镜像)。
#
# 预留槽位的簇从卷末尾往前分配,落在"镜像文件长度之外",所以
# **槽位容量在固件里是 0 字节** —— 宿主写满 PAYLOAD.BIN(128KB)也不会
# 让固件变大。真正要留存的内容走 flash payload 仓(script0/script1)。
#
# 槽位表(ramdisk_slots.h/.c)由 tools/mkfatimg.py --emit-slot-table 生成,
# 固件靠它知道每个槽位的簇位置和目录项 size 字段偏移 —— 设备端依然零 FAT 逻辑:
# 所有偏移都是构建期算好的常量,固件只做 memcpy 和定点 4 字节写。
# ---------------------------------------------------------------------------
set(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")
set(MKFATIMG "${REPO_ROOT}/tools/mkfatimg.py")
set(RAMDISK_OUT "${CMAKE_CURRENT_BINARY_DIR}/ramdisk.img")
set(SLOTS_H "${CMAKE_CURRENT_BINARY_DIR}/ramdisk_slots.h")
set(SLOTS_C "${CMAKE_CURRENT_BINARY_DIR}/ramdisk_slots.c")

# 槽位容量(字节)。改这里等于改"槽能装多大的引导文件"。
set(IO_SLOT_WATCH_PS1  32768)    # Windows 引导脚本
set(IO_SLOT_AGENT_SH   32768)    # POSIX 引导脚本
set(IO_SLOT_AGENT_EXE  65536)    # 原生可执行
set(IO_INBOX_PAYLOAD  131072)    # 宿主 -> 设备的下信箱

find_package(Python3 COMPONENTS Interpreter REQUIRED)

execute_process(
    COMMAND ${Python3_EXECUTABLE} "${MKFATIMG}"
            --out "${RAMDISK_OUT}"
            --label SOPHON
            --size 4M
            --reserve-slot "WATCH.PS1:${IO_SLOT_WATCH_PS1}"
            --reserve-slot "AGENT.SH:${IO_SLOT_AGENT_SH}"
            --reserve-slot "AGENT.EXE:${IO_SLOT_AGENT_EXE}"
            --reserve-inbox "PAYLOAD.BIN:${IO_INBOX_PAYLOAD}"
            --emit-slot-table "${SLOTS_H}"
    WORKING_DIRECTORY "${REPO_ROOT}"
    RESULT_VARIABLE _rc
    OUTPUT_VARIABLE _out
    ERROR_VARIABLE _err
)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "FAT16 image generation failed:\n${_out}\n${_err}")
endif()
# Keep status messages ASCII: idf.py pipes CMake output through the Windows
# console codec, and non-ASCII bytes there abort the build with a
# UnicodeEncodeError inside idf.py (not inside CMake).
message(STATUS "RAM disk image: ${RAMDISK_OUT}\n${_out}")

target_add_binary_data(${COMPONENT_LIB} "${RAMDISK_OUT}" BINARY)
target_sources(${COMPONENT_LIB} PRIVATE "${SLOTS_C}")
target_include_directories(${COMPONENT_LIB} PUBLIC "${CMAKE_CURRENT_BINARY_DIR}")

endif() # NOT CMAKE_SCRIPT_MODE_FILE
