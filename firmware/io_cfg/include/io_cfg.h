/*
 * io_cfg — 设备侧配置存储(NVS)
 *
 * 为什么不落明文:WiFi 密码和设备 PSK 属于凭据,**不进仓库、不进固件二进制**。
 * 它们只存在设备的 NVS 里,由串口命令(或后续的配网流程)写入。
 *
 * NVS 命名空间:"ioagent"(**绑定层,不可改** —— 改了读不到已存的 WiFi 配置)
 *   wifi_ssid  WiFi SSID
 *   wifi_pass  WiFi 密码
 *   psk        设备 PSK(后续 TLS-PSK / 鉴权用)
 *   name       设备名(mDNS 实例名的一部分)
 *   log_ips    日志服务的源 IP 授权表(逗号分隔)
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define IO_CFG_WIFI_SSID_MAX 33
#define IO_CFG_WIFI_PASS_MAX 65
#define IO_CFG_PSK_MAX        65
#define IO_CFG_NAME_MAX       32
#define IO_CFG_LOG_IPS_MAX    160

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

/**
 * 从虚拟盘上的配网文件 `DEV.CFG` 取 WiFi 凭据并写进 NVS。
 *
 * 为什么需要这个入口:设备**还没有任何网络**的时候,agent 连不上它,
 * 串口也不一定接着 —— 这时唯一能用的通道就是 **U 盘**(宿主写 → 设备读,
 * 方向天然对)。所以宿主把配置文件写到卷上,设备自己取。
 *
 * 文件格式(行式,`key=value`,`#` 开头是注释):
 *     ssid=MyNetwork
 *     pass=secret123
 *     name=sophon-lab
 *
 * 幂等:文件内容的 CRC 记在 NVS 里,**同一份文件不会重复应用**
 * (所以运行时用 MCP 改了 WiFi 之后,盘上那份旧文件不会把它改回去)。
 *
 * @param changed 置 true 表示"凭据确实变了,调用方需要重连"
 */
esp_err_t io_cfg_apply_provisioning_file(bool *changed);

/** 设备名(默认 "sophon")。 */
const char *io_cfg_device_name(void);
esp_err_t io_cfg_set_device_name(const char *name);

/**
 * 日志服务的源 IP 授权表(逗号分隔,例如 "172.17.20.17,192.168.1.5")。
 *
 * 授权判据:**能访问开发机日志的人 = 能在 DEV.CFG 里写下自己 IP 的人**。
 * 空字符串 = 没有授权表 = 日志功能整个关闭(设备端零开销)。
 */
esp_err_t io_cfg_get_log_ips(char *out, size_t len);
esp_err_t io_cfg_set_log_ips(const char *ips_csv);

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
