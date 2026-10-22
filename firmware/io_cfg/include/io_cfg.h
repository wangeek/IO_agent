/*
 * io_cfg — 设备侧配置存储(NVS)
 *
 * 为什么不落明文:WiFi 密码和设备 PSK 属于凭据,**不进仓库、不进固件二进制**。
 * 它们只存在设备的 NVS 里,由串口命令(或后续的配网流程)写入。
 *
 * NVS 命名空间:"ioagent"
 *   wifi_ssid  WiFi SSID
 *   wifi_pass  WiFi 密码
 *   psk        设备 PSK(后续 TLS-PSK / 鉴权用)
 *   name       设备名(mDNS 实例名的一部分)
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define IO_CFG_WIFI_SSID_MAX 33
#define IO_CFG_WIFI_PASS_MAX 65
#define IO_CFG_PSK_MAX        65
#define IO_CFG_NAME_MAX       32

/** 初始化 NVS(挂载 + 打开命名空间)。 */
esp_err_t io_cfg_init(void);

/** 读 WiFi 凭据。ESP_ERR_NOT_FOUND = 还没配过。 */
esp_err_t io_cfg_get_wifi(char *ssid, size_t ssid_len, char *pass, size_t pass_len);

/** 写 WiFi 凭据(空密码 = 开放网络)。 */
esp_err_t io_cfg_set_wifi(const char *ssid, const char *pass);

/** 读设备 PSK。 */
esp_err_t io_cfg_get_psk(char *psk, size_t len);

/** 写设备 PSK。 */
esp_err_t io_cfg_set_psk(const char *psk);

/** 设备名(默认 "ioagent")。 */
const char *io_cfg_device_name(void);
esp_err_t io_cfg_set_device_name(const char *name);

/**
 * 启动串口命令行(首次配网用)。
 *
 * 走 UART0(USB-TTL 的 RX 线)。命令:
 *   wifi scan             扫描附近 AP 并打印
 *   wifi set <ssid> [pass] 写入凭据(缺 pass = 开放网络)
 *   psk set <psk>          写设备 PSK
 *   name set <name>        改设备名
 *   status                 打印当前配置与运行状态
 */
esp_err_t io_cfg_start_console(void);

#ifdef __cplusplus
}
#endif
