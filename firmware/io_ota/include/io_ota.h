/*
 * io_ota — 通过 WiFi 给设备刷整机固件(OTA)。
 *
 * 为什么必须有它:
 *   单 Type-C 口的 dongle 只有一个口,插在目标机上做 HID+MSC ⇒
 *   开发机**烧不进去**了(要拔下来插开发机,再插回去,目标机那边就断连了)。
 *   所以固件更新必须能走网络。
 *
 * 形态:**设备去开发机上拉镜像**(pull),不是开发机往设备里推(push)。
 *   ① push 要在设备上收 900 KB,会把设备唯一的 httpd task 占住很久
 *      (httpd 是**单 task**,占住 = MCP 整个服务卡死)。
 *   ② pull 是标准做法(esp_https_ota 就这么干),设备边下边写另一���槽,
 *      下完校验通过才切启动分区 ⇒ **刷坏了不会变砖**(启动分区没动)。
 *   ③ 开发机只要起个静态文件服务(tools/ota_server.py)就行,不用写上传逻辑。
 *
 * 流程:
 *   1. esp_ota_get_next_update_partition() 拿到"另一个槽"
 *   2. esp_http_client 拉 URL,流式写进那个槽(esp_ota_begin/write/end)
 *   3. esp_ota_end 会校验镜像(描述符 + SHA256);失败就放弃,当前分区不动
 *   4. esp_ota_set_boot_partition() 切启动分区
 *   5. 稍等一下(让 MCP 的应答先发出去)再重启
 *
 * 安全:镜像必须和设备的分区表一致(同一个仓库构建的即是);secure boot /
 * flash encryption 关着的时候不需要签名。
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define IO_OTA_URL_MAX   192
#define IO_OTA_ERR_MAX   96

/** OTA 进度/结果。 */
typedef struct {
    char state[16];              /**< idle | downloading | staged | rebooting | failed */
    char url[IO_OTA_URL_MAX];    /**< 正在/已经处理的镜像地址 */
    int32_t received;            /**< 已收字节 */
    int32_t total;               /**< 镜像总长(-1 = 未知) */
    char error[IO_OTA_ERR_MAX];   /**< 失败原因 */
    char boot[18];               /**< 当前正在跑的分区标签(ota_0 / ota_1) */
    char next[18];               /**< 下一次 OTA 会写进去的分区标签 */
} io_ota_info_t;

/**
 * 开始一次 OTA(异步)。
 *
 * 立即返回;真正的下载在后台任务里跑。刷完后设备会**自己重启**
 * (所以发起这次调用的 MCP 连接会断 —— 这是正常的,agent 重新发现设备即可)。
 *
 * @param url 镜像地址,如 http://192.168.1.5:8000/sophon.bin
 */
esp_err_t io_ota_start(const char *url);

/** 当前是否有一个 OTA 在跑。 */
bool io_ota_busy(void);

/** 取 OTA 状态(给 status 工具用)。 */
esp_err_t io_ota_get_info(io_ota_info_t *out);

/**
 * 固件身份(来自 ESP-IDF 的 app 描述符)。
 *
 * 刷完固件后 agent 靠它确认"设备上的固件真的变了" —— 比看 uptime 可靠。
 */
typedef struct {
    char version[32];
    char idf_ver[32];
    char project[32];
    char build[32];        /**< 编译时间(来自 app 描述符的 date 字段) */
} io_ota_build_t;

/** 取当前固件的版本/编译时间。 */
esp_err_t io_ota_get_build(io_ota_build_t *out);

#ifdef __cplusplus
}
#endif
