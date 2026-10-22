idf_component_register(
    SRCS "io_launcher.c"
    INCLUDE_DIRS "include"
    REQUIRES io_disk io_hid io_usb
)

# 自动拉起开关。
#
# **默认 0(关)**:开机自动往目标机注入按键是对用户环境的破坏性操作 ——
# 目标机若处于锁屏,那些按键会被锁屏界面当成 PIN 输入,持续失败会触发
# Windows 的账户防暴力锁定(2026-10-22 实测事故)。
#
# 拉起方式改为**远程显式调用**(MCP `act` 工具):有人明确下令、单次执行、零重试。
# 1 = 保留旧行为(仅供受控环境下的实验),**不要在插着用户主力机时打开**。
target_compile_definitions(${COMPONENT_LIB} PUBLIC IO_LAUNCHER_AUTOSTART=0)
