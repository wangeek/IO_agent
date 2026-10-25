/*
 * io_log — 把设备日志通过 **WiFi** 暴露给开发机(UDP + 源 IP 白名单)。
 *
 * 为什么要它:
 *   单 Type-C 口的 dongle 没有第二个 USB 口 ⇒ 开发板上那块 CH343 桥就没了,
 *   "插开发机看日志"这条路直接断掉。日志必须能从网络看到,否则每次烧录
 *   都得靠猜。
 *
 * 形态:**设备是 UDP server,开发机主动发包来"要"日志,设备把日志回过去**。
 *
 *   为什么不是"设备 UDP 广播到开发机":
 *     ① **Windows 防火墙**。开发机的网络位置通常是 Public,入站默认拦。
 *        设备主动发包是 **unsolicited 入站** ⇒ 每台开发机都要管理员加规则。
 *        而这里开发机**先发一个包**,设备的回包走同一条会话(回程流量),
 *        Windows 的状态化防火墙放行 ⇒ **一条防火墙规则都不用加**。
 *     ② **广播本身不可靠**。AP 的 IGMP snooping / 客户端隔离都可能把广播丢掉,
 *        而且丢了你完全不知道(设备侧 sendto 照样返回成功)。
 *
 * 授权怎么解决(不靠加密、不靠密钥):
 *   设备端**预先配置一个允许的源 IP 列表**(allowlist,与 ssid 一并由宿主
 *   写进 U 盘 DEV.CFG 的 `logips=` 键)。收到 UDP 包先比源 IP:
 *   **不在列表里就直接丢,连回包都不给**。
 *   ⇒ 局域网里别的机器问多少次都拿不到日志,而且设备侧连"有人在问"都不暴露。
 *   ⇒ **没配 logips = 日志功能整个不开**(连 UDP socket 都不建,vprintf 钩子
 *     也不装)⇒ 量产固件的日志面是零。
 *   判据:**能访问开发机日志的人 = 能在 DEV.CFG 里写下自己 IP 的人**。
 *
 * 开销:一个低优先级 task 常驻在 recvfrom 上(没有包就睡着),一个 8 KB
 *   环形缓冲,一次 IP 比较。**没有 httpd、没有第二个协议栈、没有加密**。
 *
 * 钩子:ESP-IDF 的日志全部经过一个 vprintf 钩子(`esp_log_set_vprintf`),
 *   我们把钩子换成自己的:UART 那一路照旧(串口没了只是没人看,行为不变),
 *   同一份字节再塞进一个环形缓冲。**钩子里绝不做可能阻塞的操作**
 *   (它可能被任何任务调用,包括注入时序敏感的任务)—— 只 memcpy。
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 日志服务的 UDP 端口(独立于 MCP 的 8080)。 */
#define IO_LOG_PORT        8090
/** allowlist 字符串上限(逗号分隔的 IP 列表)。 */
#define IO_LOG_IPS_MAX     160
/** 单个 UDP 帧的负载上限(留 IP/UDP 头余量,别超 1472)。 */
#define IO_LOG_FRAME_MAX   1400

/**
 * 启动网络日志(UDP server + 源 IP allowlist)。
 *
 * @param ips_csv 允许的源 IP 列表,逗号分隔,例如 "172.17.20.17,192.168.1.5"。
 *                **空 / 非法 ⇒ 不启动**(日志功能关闭,零开销)。
 *
 * 装上 vprintf 钩子(UART 输出保持不变)+ 建 UDP socket + 起接收 task。
 * 网络还没起来也能调 —— 日志会先在环形缓冲里等着。
 */
esp_err_t io_log_net_start(const char *ips_csv);

/**
 * 真正开 socket 收包(第一阶段 net_start 只装钩子攒日志)。
 *
 * 为什么分两段:**socket 收包和 WiFi 刚连上那段时间会互相踩** ——
 * ESP32-S3 开机时 Core 1 要做 RF 校准(读 eFuse ⇒ flash 操作),而
 * Core 0 若此时正好在 socket 接收的临界区里等 portMux,就死锁成
 * `Interrupt wdt timeout`(实测炸过一次)。所以 socket 等 WiFi 连上
 * 之后再开,钩子照旧先装 ⇒ 开机日志照样攒得住,第一次请求就能补发。
 */
esp_err_t io_log_net_open(void);

/**
 * 停掉网络日志(关 socket + 撤钩子 + 退任务)。
 *
 * 为什么需要"停"这个动作:授权表是**源 IP 白名单**,而开发机的 IP 是
 * DHCP 分配的 —— 换网段、换 WiFi、笔记本换热点都会变。停掉再起就能
 * **热切换授权表**,不用重启设备。
 */
esp_err_t io_log_stop(void);

/** 日志服务的端口(给 status 用)。 */
uint16_t io_log_port(void);

/** 日志服务是否已启用(没配 IP 授权表就是 false)。 */
bool io_log_active(void);

/** socket 是否已经打开(开始收包了)。 */
bool io_log_opened(void);

/** allowlist 里有几个 IP。 */
uint32_t io_log_allow_count(void);

/** 诊断:收到过多少个 UDP 包。 */
uint32_t io_log_rx_packets(void);

/** 诊断:其中有多少个因为源 IP 不在 allowlist 被丢掉。 */
uint32_t io_log_rx_rejected(void);

/** 诊断:回出去多少个包。 */
uint32_t io_log_tx_packets(void);

/** 诊断:回出去多少字节。 */
uint32_t io_log_tx_bytes(void);

/** 诊断:日志钩子被调用了多少次(>0 说明钩子装上了)。 */
uint32_t io_log_hook_calls(void);

#ifdef __cplusplus
}
#endif
