/*
 * io_usb — USB 复合设备栈对外接口
 *
 * 组合形态(方案 A′):
 *   HID 接口:Report ID 1 = 键盘,Report ID 2 = 鼠标(单接口复合描述符)
 *   MSC 接口:块设备,介质是 io_disk 的 PSRAM RAM disk
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 启动 USB 复合设备栈(PHY + TinyUSB 任务 + 描述符)。 */
esp_err_t io_usb_init(void);

/** 主机是否已枚举并完成 SetConfiguration(设备已"上线")。
 *
 *  注意:这是**历史事实**(枚举成功过且未收到 DETACHED),不代表"此刻还连着"。
 *  判断当前链路请用 io_usb_link_alive()。
 */
bool io_usb_host_configured(void);

/** 链路此刻是否可用(可以注入 / 可以收发)。
 *
 *  = tud_ready() = tud_mounted() && !tud_suspended()
 *
 *  为什么不用 TINYUSB_EVENT_DETACHED 判断拔线:
 *  ESP32-S3 的 USB 外设是 Synopsys DWC2 内核,TinyUSB 的 DWC2 device 模式
 *  **没有实现设备断开检测**——src/portable/synopsys/dwc2/dcd_dwc2.c 里
 *  "GINTSTS_DISCINT for disconnect detection" 仍是 TODO,唯一的
 *  DCD_EVENT_UNPLUGGED 来源 GOTGINT_SEDET 需要 OTG + VBUS 感知,
 *  devkit 上没有接。物理拔线时 DWC2 实际上报的是 SUSPEND
 *  (拔线瞬间 D+/D- 变 SE0,总线空闲 3ms 即满足 suspend 条件),
 *  于是 suspended=1 -> tud_ready()=false。
 *
 *  恢复路径可靠:重新插上后主机发 SOF,SOF 处理器会清掉 suspended
 *  (tinyusb/src/device/usbd.c 的 DCD_EVENT_SOF 分支)。
 */
bool io_usb_link_alive(void);

/** 主机是否已 SetConfiguration(枚举完成)。 */
bool io_usb_mounted(void);

/** USB 总线是否处于 suspend 状态。 */
bool io_usb_suspended(void);

#ifdef __cplusplus
}
#endif
