/*
 * io_net — WiFi STA + mDNS
 *
 * 拓扑(见 docs/connectivity-and-binding.md):设备是 **MCP 服务端**,
 * agent 主动连设备。设备侧只需要:上 WiFi、把服务用 mDNS 广播出去、
 * 拿到自己的 IP。
 *
 * 广播名:`_ioagent._tcp.local`,实例名 = 设备名(NVS 里配的)。
 * agent 用 mDNS 找到设备 -> 直连设备的 MCP 端点。
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 初始化网络栈(不连接)。 */
esp_err_t io_net_init(void);

/**
 * 用给定凭据连接 WiFi,并启动自动重连。
 * 凭据为空时用 NVS 里存的。
 */
esp_err_t io_net_connect(const char *ssid, const char *pass);

/** 是否已连上 WiFi 并拿到 IP。 */
/**
 * 用新的凭据(或 NVS 里已有的)重连 WiFi。
 *
 * 与 io_net_connect() 的区别:这个函数**不会**因为 esp_wifi_start() 失败而 abort
 * —— WiFi 栈早就启动过了,重复 start 返回错误是正常的。
 * 配网文件(DEV.CFG)变化后用它重连。
 */
esp_err_t io_net_reconnect(const char *ssid, const char *pass);

bool io_net_is_connected(void);

/**
 * 有没有发起过连接尝试。
 *
 * 用来区分两种"网络没就绪":刚上电、还没开始连(LED:启动中)vs.
 * 已经在连/连不上(LED:网络未就绪)。设备自己能测出来。
 */
bool io_net_connect_started(void);

/** 当前 IP(字符串,没连上返回 "0.0.0.0")。 */
const char *io_net_ip(void);

/** 启动 mDNS 广播(_ioagent._tcp.local,端口 port)。 */
esp_err_t io_net_start_mdns(uint16_t port);

/** 断开 WiFi(省电/受控场景)。 */
esp_err_t io_net_disconnect(void);

#ifdef __cplusplus
}
#endif
