/*
 * IO_Agent 固件入口
 *
 * 职责分层:
 *   1. 把出厂的 FAT16 镜像装进 PSRAM RAM disk(设备端的"随身工具仓库")
 *   2. 起 USB 复合设备栈(HID 键鼠 + MSC 虚拟 U 盘)
 *   3. T2:flash payload 仓 —— 开机把最新一份有效快照覆盖到 RAM disk 槽位
 *   4. M5 远程链路:WiFi + mDNS + MCP server(agent 主动连过来)
 *
 * **注入默认是关的**(io_launcher 的 IO_LAUNCHER_AUTOSTART=0):
 * 往用户机器注入按键是对用户环境的破坏性操作,只允许经由远程 `act` 工具
 * 显式触发(单次、零重试)。见 firmware/io_launcher/CMakeLists.txt。
 */
#include <stdio.h>
#include <string.h>

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "io_disk.h"
#include "io_usb.h"
#include "io_hid.h"
#include "io_payload.h"
#include "io_launcher.h"
#include "io_cfg.h"
#include "io_net.h"
#include "io_mcp.h"

static const char *TAG = "io_agent";

/* 链路跃迁的轮询周期:要足够快,才能抓住"拔线 -> 等 3 秒 -> 插回"这种短窗口 */
#define LINK_POLL_MS      200
/* 完整状态行的打印周期(每 LINK_POLL_MS 一次,25 次 = 5 秒) */
#define STATUS_PERIOD_N   25

/* 等 WiFi 连上之后启动 mDNS 广播 */
static void netboot_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (io_net_is_connected()) {
            ESP_LOGI(TAG, "starting mDNS on %s", io_net_ip());
            io_net_start_mdns(IO_MCP_PORT);
            vTaskDelete(NULL);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

static void status_task(void *arg)
{
    (void)arg;
    uint32_t last_blocks = 0;
    uint32_t tick = 0;
    int last_link = -1;      /* -1 = 尚未取过样,强制首次打一行 */

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(LINK_POLL_MS));

        /*
         * 链路活性判据用 tud_ready()(mounted && !suspended),不用 DETACHED 事件:
         * ESP32-S3 的 DWC2 device 模式不实现设备断开检测,物理拔线只报 SUSPEND,
         * TINYUSB_EVENT_DETACHED 永远不会触发(详见 include/io_usb.h)。
         */
        const bool link_alive = io_usb_link_alive();
        const int link = link_alive ? 1 : 0;

        if (link != last_link) {
            if (last_link < 0) {
                ESP_LOGI(TAG, "link state: %d (mounted=%d suspended=%d)",
                         link, (int)io_usb_mounted(), (int)io_usb_suspended());
            } else if (link == 0) {
                ESP_LOGW(TAG, "host link lost (mounted=%d suspended=%d) -- cable removed or host suspended",
                         (int)io_usb_mounted(), (int)io_usb_suspended());
            } else {
                ESP_LOGI(TAG, "host link restored (mounted=%d suspended=%d)",
                         (int)io_usb_mounted(), (int)io_usb_suspended());
            }
            last_link = link;
        }

        if (++tick < STATUS_PERIOD_N) {
            continue;
        }
        tick = 0;

        const uint32_t blocks = io_disk_write_stats_blocks();
        const uint64_t bytes = io_disk_write_stats_bytes();

        ESP_LOGI(TAG,
                 "status: link=%d mounted=%d suspended=%d enumerated=%d hid_ready=%d "
                 "sectors=%u written=%u blocks / %llu bytes",
                 link,
                 (int)io_usb_mounted(),
                 (int)io_usb_suspended(),
                 (int)io_usb_host_configured(),
                 (int)io_hid_ready(),
                 io_disk_sector_count(),
                 blocks,
                 (unsigned long long)bytes);

        if (blocks != last_blocks) {
            ESP_LOGI(TAG, "host write detected: %u blocks", (unsigned)(blocks - last_blocks));
            last_blocks = blocks;
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, " IO_Agent boot (ESP32-S3 N16R8)");
    ESP_LOGI(TAG, "========================================");

    ESP_ERROR_CHECK(io_disk_init());
    ESP_ERROR_CHECK(io_hid_init());
    ESP_ERROR_CHECK(io_usb_init());

    /* T2:flash payload 仓。开机把最新一份有效快照覆盖到 RAM disk 槽位
       (出厂态没有快照 -> 保持出厂镜像)。 */
    const esp_err_t pl = io_payload_init();
    if (pl != ESP_OK && pl != ESP_ERR_NOT_FOUND) {
        ESP_LOGE(TAG, "payload init failed: %s", esp_err_to_name(pl));
    }
    ESP_ERROR_CHECK(io_payload_start_ingest());

    /* M2:注入链路。默认**不自动拉起**(IO_LAUNCHER_AUTOSTART=0)——
       拉起只能由远程 `act` 工具显式触发。 */
    ESP_ERROR_CHECK(io_launcher_start());

    /* M5 远程链路:配置 → WiFi → HTTP(MCP)→ 等 IP 后广播 mDNS
     *
     * ★ 没有 WiFi 凭据**不是致命错误**:设备还可以在纯 USB 模式下工作,
     * 等人用串口控制台配网。绝不能用 ESP_ERROR_CHECK —— 那会变成
     * panic 重启循环(踩过:没配网就无限重启,连串口命令都执行不了)。 */
    ESP_ERROR_CHECK(io_cfg_init());
    ESP_ERROR_CHECK(io_cfg_start_console());
    ESP_ERROR_CHECK(io_net_init());
    const esp_err_t nc = io_net_connect(NULL, NULL);
    if (nc != ESP_OK) {
        ESP_LOGW(TAG, "WiFi not configured yet (%s). Use the UART console: "
                 "'wifi scan' / 'wifi set <ssid> [pass]'", esp_err_to_name(nc));
    }
    ESP_ERROR_CHECK(io_mcp_start());
    xTaskCreate(netboot_task, "netboot", 3072, NULL, 3, NULL);

    xTaskCreate(status_task, "status", 3072, NULL, 3, NULL);

    ESP_LOGI(TAG, "init done, waiting for host enumeration");
}
