/*
 * Sophon 固件入口
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
#include "io_log.h"
#include "io_led.h"
#include "io_lcd.h"
#include "io_mcp.h"

static const char *TAG = "sophon";

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
            /*
             * 日志服务的 socket **等 WiFi 连上再开**。
             * ESP32-S3 开机时 Core 1 要做 RF 校准(读 eFuse ⇒ flash 操作),
             * 而 Core 0 若此时正好阻塞在 socket 接收的临界区里等 portMux,
             * 就死锁成 `Interrupt wdt timeout`(实测炸过一次)。
             * 钩子是早就装好的,所以这段开机日志照样攒在环里,第一次请求能补发。
             */
            if (io_log_active()) {
                io_log_net_open();
            }
            vTaskDelete(NULL);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

/*
 * 配网轮询:宿主把 DEV.CFG 写到虚拟盘上,设备每 5 秒看一眼,
 * 内容变了就写进 NVS 并重连。
 *
 * 为什么要有这条:设备**还没有任何网络**的时候 agent 连不上它、串口也未必
 * 接着 —— U 盘是唯一方向天然对的通道(宿主写 → 设备读)。
 * 顺带它也是"运行时换网"的路径:写文件即可,不用重启。
 */
static void prov_task(void *arg)
{
    (void)arg;
    for (;;) {
        bool changed = false;
        if (io_cfg_apply_provisioning_file(&changed) == ESP_OK && changed) {
            ESP_LOGI(TAG, "provisioning file changed -> reconnecting WiFi");
            io_net_reconnect(NULL, NULL);
            /*
             * 日志服务的授权表也可能刚被改过。服务是**开机时**按 NVS 里的
             * 授权表起的,所以这里补一刀:还没起就现在起(开发者写完
             * DEV.CFG 不用重启就能看到日志)。
             * 已经起着的**改授权表要重启**才生效(换 IP 授权属于安全相关
             * 变更,不做热切换)。
             */
            char ips[IO_CFG_LOG_IPS_MAX] = {0};
            if (io_cfg_get_log_ips(ips, sizeof(ips)) == ESP_OK && ips[0]) {
                if (!io_log_active()) {
                    if (io_log_net_start(ips) == ESP_OK) {
                        io_log_net_open();
                        ESP_LOGI(TAG, "network log started (allowlist from provisioning file)");
                    }
                } else {
                    ESP_LOGW(TAG, "log allowlist changed -- restart the device to apply");
                }
            } else if (io_log_active()) {
                ESP_LOGW(TAG, "log allowlist cleared -- restart the device to disable it");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
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

/*
 * 屏:单独一个 task。
 *
 * 为什么不在 app_main 里直接调:io_lcd_init() 要等 ST7735S 的复位/唤醒时序
 * (SWRESET 后 150 ms、SLPOUT 后 500 ms),放在主流程里会把 USB 枚举
 * 也一起拖住 700 ms —— 枚举越晚,宿主越晚认到设备。
 */
static void lcd_task(void *arg)
{
    (void)arg;
    const esp_err_t lrc = io_lcd_init();
    if (lrc != ESP_OK) {
        /* 屏起不来不该拖垮设备:它是显示件,不是数据通路 */
        ESP_LOGW(TAG, "LCD unavailable (%s) -- continuing", esp_err_to_name(lrc));
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "LCD ready: %dx%d", io_lcd_width(), io_lcd_height());
    /*
     * 开机画一组彩条。
     *
     * 屏的**角色**还没定(见 docs/screen-decision.md),而彩条是唯一能一眼
     * 分清三件事的图案:红蓝有没有反(BGR 位错)、GRAM 偏移对不对(偏移错
     * 会露出黑边)、旋转对不对(横竖反)。
     */
    io_lcd_selftest();
    vTaskDelete(NULL);
}

void app_main(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, " Sophon boot (ESP32-S3 N16R8)");
    ESP_LOGI(TAG, "========================================");

    /*
     * 状态 LED 最早起来:它是**给人看的**(现场一眼看出"设备什么状态"),
     * 所以要在连网之前就位 —— 启动阶段正好显示"蓝色快闪"。
     * 编译期开关 CONFIG_SOPHON_LED_WS2812 关掉时这里什么都不做。
     */
    {
        const esp_err_t lrc = io_led_init();
        if (lrc != ESP_OK) {
            ESP_LOGW(TAG, "status LED unavailable (%s) -- continuing",
                     esp_err_to_name(lrc));
        }
    }

    /* 屏:编译期开关 CONFIG_SOPHON_LCD_ST7735 关掉时这里什么都不做 */
    xTaskCreate(lcd_task, "lcd", 3072, NULL, 3, NULL);

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

    /*
     * 配网:开机先取一次盘上的 DEV.CFG(有就写进 NVS),
     * 这样第一次连接就用上新的凭据,不用等 5 秒轮询。
     */
    bool prov_changed = false;
    if (io_cfg_apply_provisioning_file(&prov_changed) == ESP_OK && prov_changed) {
        ESP_LOGI(TAG, "provisioning applied from DEV.CFG before connecting");
    }

    ESP_ERROR_CHECK(io_net_init());
    const esp_err_t nc = io_net_connect(NULL, NULL);
    if (nc != ESP_OK) {
        ESP_LOGW(TAG, "WiFi not configured yet (%s). Use the UART console: "
                 "'wifi scan' / 'wifi set <ssid> [pass]'", esp_err_to_name(nc));
    }

    /*
     * 网络日志:日志同时送 UART 和 WiFi(UDP + 源 IP 授权表)。
     * 单 Type-C 口的 dongle 没有第二个 USB 口 ⇒ 串口这条路会消失,
     * 日志必须能从网络看到,否则每次烧录都只能靠猜。
     *
     * **授权**:设备端有一张预先配置好的源 IP 白名单(DEV.CFG 的 `logips=`),
     * 收到 UDP 包先比源 IP,不在表里就直接丢、连回包都不给。
     * **没配白名单 ⇒ 这个服务整个不开**(socket 不建、钩子不装)⇒ 量产固件
     * 的日志面和开销都是零。
     *
     * 装钩子放在 mcp_start 之前,这样后面的启动日志也能进网络缓冲。
     *
     * ★ 这里**不能**用 ESP_ERROR_CHECK ★
     *   日志服务是**调试设施**,不是关键路径 —— 它起不来(端口被占、
     *   内存不够)不该把整台设备变成砖。失败只警告,MCP 照常服务。
     */
    {
        char ips[IO_CFG_LOG_IPS_MAX] = {0};
        esp_err_t lrc = ESP_ERR_INVALID_ARG;
        if (io_cfg_get_log_ips(ips, sizeof(ips)) == ESP_OK && ips[0]) {
            lrc = io_log_net_start(ips);
            if (lrc != ESP_OK) {
                ESP_LOGW(TAG, "network log unavailable (%s) -- UART logs still work",
                         esp_err_to_name(lrc));
            }
        } else {
            ESP_LOGI(TAG, "no log allowlist in NVS -- network log off");
        }
    }
    /*
     * MCP server 起不来**不能 abort**。
     * abort = 立刻重启 = 再起不来 = 再重启 ⇒ 无限重启循环,设备彻底死掉,
     * 连串口控制台都用不上(只有一行日志)。而 MCP 挂了的时候,USB 的
     * HID/MSC 和 UART 控制台**都还活着** —— 那正是排查它需要的通道。
     * 所以:记一条错误,设备继续跑。
     * (这个坑踩过:cfg.max_open_sockets 设成 16,超过 IDF 的上限
     *  (LWIP_MAX_SOCKETS-3)⇒ httpd_start 失败 ⇒ ESP_ERROR_CHECK ⇒ 砖。
     *  同一个 sdkconfig 改大 LWIP_MAX_SOCKETS 才是正解。)
     */
    {
        const esp_err_t mrc = io_mcp_start();
        if (mrc != ESP_OK) {
            ESP_LOGE(TAG, "MCP server failed to start (%s) -- continuing without it",
                     esp_err_to_name(mrc));
        }
    }
    xTaskCreate(netboot_task, "netboot", 3072, NULL, 3, NULL);
    xTaskCreate(prov_task, "prov", 3072, NULL, 3, NULL);

    xTaskCreate(status_task, "status", 3072, NULL, 3, NULL);

    ESP_LOGI(TAG, "init done, waiting for host enumeration");
}
