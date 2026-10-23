#include "io_net.h"

#include <string.h>

#include <esp_log.h>
#include <esp_event.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <esp_check.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "io_cfg.h"

static const char *TAG = "io_net";

#define WIFI_RETRY_MIN_MS   2000
#define WIFI_RETRY_MAX_MS  30000

static bool     s_connected;
static bool     s_connecting;
static bool     s_has_creds;     /* 有没有凭据:没有就别重试,重试也没意义 */
static char     s_ip[16] = "0.0.0.0";
static uint32_t s_retry_ms = WIFI_RETRY_MIN_MS;
static esp_netif_t *s_sta_netif;

static void retry_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (!s_connected && !s_connecting && s_has_creds) {
            ESP_LOGW(TAG, "reconnecting to WiFi in %u ms ...", (unsigned)s_retry_ms);
            vTaskDelay(pdMS_TO_TICKS(s_retry_ms));
            s_connecting = true;
            esp_wifi_connect();
        } else {
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id,
                               void *data)
{
    (void)arg;
    (void)data;

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "STA started, connecting ...");
        s_connecting = true;
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        s_connecting = false;
        strcpy(s_ip, "0.0.0.0");
        ESP_LOGW(TAG, "disconnected (reason=%d)", (int)0);
        /* 退避重连:失败次数越多间隔越长,避免疯狂重试刷爆路由器 */
        s_retry_ms = (s_retry_ms * 2 > WIFI_RETRY_MAX_MS) ? WIFI_RETRY_MAX_MS : s_retry_ms * 2;
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
        s_connected = true;
        s_connecting = false;
        s_retry_ms = WIFI_RETRY_MIN_MS;
        ESP_LOGI(TAG, "got IP: %s", s_ip);
    }
}

esp_err_t io_net_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_sta_netif = esp_netif_create_default_wifi_sta();
    if (!s_sta_netif) {
        ESP_LOGE(TAG, "esp_netif_create_default_wifi_sta failed");
        return ESP_FAIL;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        wifi_event_handler, NULL, NULL));

    if (xTaskCreate(retry_task, "wifi_retry", 3072, NULL, 3, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t io_net_connect(const char *ssid, const char *pass)
{
    char s[IO_CFG_WIFI_SSID_MAX];
    char p[IO_CFG_WIFI_PASS_MAX];

    if (ssid && ssid[0]) {
        strncpy(s, ssid, sizeof(s) - 1);
        s[sizeof(s) - 1] = '\0';
        strncpy(p, (pass && pass[0]) ? pass : "", sizeof(p) - 1);
        p[sizeof(p) - 1] = '\0';
    } else if (io_cfg_get_wifi(s, sizeof(s), p, sizeof(p)) != ESP_OK) {
        ESP_LOGW(TAG, "no WiFi credentials configured");
        s_has_creds = false;
        return ESP_ERR_NOT_FOUND;
    }
    s_has_creds = true;

    wifi_config_t wc = { 0 };
    strncpy((char *)wc.sta.ssid, s, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, p, sizeof(wc.sta.password) - 1);
    wc.sta.threshold.authmode = (p[0] == '\0') ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;

    ESP_LOGI(TAG, "connecting to \"%s\" ...", s);
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    return ESP_OK;
}

esp_err_t io_net_reconnect(const char *ssid, const char *pass)
{
    char s[IO_CFG_WIFI_SSID_MAX];
    char p[IO_CFG_WIFI_PASS_MAX];

    if (ssid && ssid[0]) {
        strncpy(s, ssid, sizeof(s) - 1);
        s[sizeof(s) - 1] = '\0';
        strncpy(p, (pass && pass[0]) ? pass : "", sizeof(p) - 1);
        p[sizeof(p) - 1] = '\0';
    } else if (io_cfg_get_wifi(s, sizeof(s), p, sizeof(p)) != ESP_OK) {
        s_has_creds = false;
        return ESP_ERR_NOT_FOUND;
    }
    s_has_creds = true;

    wifi_config_t wc = { 0 };
    strncpy((char *)wc.sta.ssid, s, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, p, sizeof(wc.sta.password) - 1);
    wc.sta.threshold.authmode = (p[0] == '\0') ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;

    ESP_LOGI(TAG, "reconnecting to \"%s\" ...", s);
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wc), TAG, "set config failed");

    /*
     * esp_wifi_start() 在已经启动时会返回错误 —— 那是**正常**的,
     * 不能像 io_net_connect() 那样 ESP_ERROR_CHECK(直接 abort)。
     * 这里只关心"配置换掉了",所以启动失败也继续走重连。
     */
    const esp_err_t started = esp_wifi_start();
    if (started != ESP_OK) {
        ESP_LOGD(TAG, "wifi already started (%s)", esp_err_to_name(started));
    }
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(200));
    s_connected = false;
    s_connecting = true;
    s_retry_ms = WIFI_RETRY_MIN_MS;
    esp_wifi_connect();
    return ESP_OK;
}

bool io_net_is_connected(void)
{
    return s_connected;
}

const char *io_net_ip(void)
{
    return s_ip;
}

esp_err_t io_net_start_mdns(uint16_t port)
{
    /*
     * mDNS 发现机制 —— **M5 收尾项,当前未实现**。
     *
     * 原因:ESP-IDF 5.5 没有内置 mDNS 组件(esp-mdns 是组件管理器里的第三方库,
     * 而本项目关闭了组件管理器以保证离线可复现构建);lwIP 自带的 MDNS
     * responder 也没启用(CONFIG_LWIP_MDNS_RESPONDER 未开)。
     *
     * 候选实现(择一,届时再定):
     *   a) 开 CONFIG_LWIP_MDNS_RESPONDER,用 lwIP 原生 mdns API(apps/mdns.h)
     *   b) vendored esp-mdns 库(和 esp_tinyusb 一样随仓库走)
     *
     * 现阶段 agent 用串口日志里打印的 IP 直连即可(开发测试够用)。
     */
    ESP_LOGI(TAG, "MCP service reachable at http://%s:%u/  (mDNS not enabled yet)",
             io_net_ip(), (unsigned)port);
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t io_net_disconnect(void)
{
    return esp_wifi_disconnect();
}
