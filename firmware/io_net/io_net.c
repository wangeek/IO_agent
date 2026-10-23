#include "io_net.h"

#include <string.h>

#include <esp_log.h>
#include <esp_event.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <esp_check.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "lwip/netif.h"
#include "lwip/err.h"
#include "lwip/tcpip.h"
#include "lwip/apps/mdns.h"

#include "io_cfg.h"

static const char *TAG = "io_net";

#define WIFI_RETRY_MIN_MS   2000
#define WIFI_RETRY_MAX_MS  30000

static bool     s_connected;
static bool     s_connecting;
static bool     s_has_creds;     /* 有没有凭据:没有就别重试,重试也没意义 */
static bool     s_connect_started; /* 已经发起过连接(LED 用:区分"启动中"和"没连上") */
static char     s_ip[16] = "0.0.0.0";
static uint32_t s_retry_ms = WIFI_RETRY_MIN_MS;
static esp_netif_t *s_sta_netif;
static bool     s_mdns_started;   /* mDNS responder 只装配一次 */

/*
 * mDNS 装配要在 TCP/IP 线程里做(原因见 io_net_start_mdns 的注释),
 * 所以上下文要跨线程传,用这个结构体。
 */
#define IO_MDNS_NAME_MAX 32

typedef struct {
    char           name[IO_MDNS_NAME_MAX];
    uint16_t       port;
    int            rc;            /* 0 = ok,负数 = 失败原因 */
    volatile bool  done;
} io_mdns_ctx_t;

/* 拿到 STA 对应的 lwIP netif。挑"已 up 且拿到 IPv4"的那个,别去猜 netif_list 的顺序。 */
static struct netif *io_net_sta_lwip_netif(void)
{
    struct netif *fallback = NULL;
    for (struct netif *nif = netif_list; nif != NULL; nif = nif->next) {
        if (!fallback) {
            fallback = nif;
        }
        if (netif_is_up(nif) && !ip4_addr_isany_val(*netif_ip4_addr(nif))) {
            return nif;
        }
    }
    return fallback;
}

/* 在 TCP/IP 线程里执行:真正的 mDNS 装配。 */
static void io_net_mdns_setup_cb(void *arg)
{
    io_mdns_ctx_t *c = (io_mdns_ctx_t *)arg;

    struct netif *sta = io_net_sta_lwip_netif();
    if (!sta) {
        ESP_LOGE(TAG, "mDNS: no lwIP netif");
        c->rc = -1;
        c->done = true;
        return;
    }

    mdns_resp_init();
    if (mdns_resp_add_netif(sta, c->name) != ERR_OK) {
        ESP_LOGE(TAG, "mDNS: add_netif failed");
        c->rc = -2;
        c->done = true;
        return;
    }
    if (mdns_resp_add_service(sta, c->name, "_ioagent", DNSSD_PROTO_TCP, c->port,
                              NULL, NULL) != ERR_OK) {
        ESP_LOGE(TAG, "mDNS: add_service failed");
        c->rc = -3;
        c->done = true;
        return;
    }
    mdns_resp_announce(sta);

    c->rc = 0;
    c->done = true;
}

/*
 * 重新广播:DHCP 换了 IP 之后,旧的 A 记录就是过期地址了。
 * 同样要在 TCP/IP 线程里做。
 */
static void io_net_mdns_announce_cb(void *arg)
{
    (void)arg;
    struct netif *sta = io_net_sta_lwip_netif();
    if (sta) {
        mdns_resp_netif_settings_changed(sta);
    }
}

bool io_net_connect_started(void)
{
    return s_connect_started;
}

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
        /* IP 变了 -> 重新广播 mDNS(否则 A 记录指向旧地址) */
        if (s_mdns_started) {
            tcpip_callback(io_net_mdns_announce_cb, NULL);
        }
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
    s_connect_started = true;

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
     * mDNS 发现(2026-10-23 实现)。
     *
     * 作用:让 agent **不用配 IP 就能找到设备** —— 设备 IP 是 DHCP 分配的,
     * 重启/换网后会变。设备把 <设备名>.local 与服务类型 _ioagent._tcp.local
     * 广播出去,agent 解析出 IP:8080 就能连。
     *
     * 为什么用 lwIP 原生 responder:esp-mdns 在组件管理器里(与"关组件管理器 +
     * 离线可复现"冲突),而 lwIP 自带的 mdns app 支持 DNS-SD 服务记录 + 端口 +
     * TXT,正好够用 —— 我们把 IDF 树里那三个 .c 直接编进本组件
     * (见 io_net/CMakeLists.txt),不 vendored 副本,始终跟 IDF 版本一致。
     *
     * 注意:lwIP 的 mdns app 需要 netif 的 client_data 槽位,所以 sdkconfig 里
     * 打开了 CONFIG_LWIP_NUM_NETIF_CLIENT_DATA(整个 lwip 统一编译,不会错位)。
     */
#if LWIP_MDNS_RESPONDER
    if (s_mdns_started) {
        return ESP_OK;
    }
    s_mdns_started = true;

    io_mdns_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.port = port;
    ctx.rc = -99;
    /* 名字复制一份:回调在 TCP/IP 线程里跑,不能依赖调用方的缓冲区 */
    snprintf(ctx.name, sizeof(ctx.name), "%s", io_cfg_device_name());

    /*
     * ★ 必须在 TCP/IP 线程里装配 ★
     *
     * mdns_resp_add_netif() -> mdns_resp_restart() -> mdns_set_timeout() ->
     * sys_timeout():往 lwIP 的定时器链表里插条目。而 lwIP 核心(含定时器链表)
     * 跑在 TCP/IP 线程上,本项目的 sdkconfig **没有开** CORE_LOCKING
     * (CONFIG_LWIP_TCPIP_CORE_LOCKING is not set),所以从别的任务插条目是数据竞争。
     *
     * 症状极具迷惑性:所有 mdns API 都返回成功,日志也照打,但:
     *   - 定时器不触发 -> 状态机永远停在 MDNS_STATE_PROBING
     *   - 停在 PROBING 就不会发探测包(线上一个包都没有)
     *   - mdns.c 的 mdns_handle_question() 在 state != COMPLETE 时直接 return
     *     -> 收到查询也不应答
     * 表现就是"注册成功但永远不应答"。
     *
     * 所以:用 tcpip_callback() 把装配动作投递到 TCP/IP 线程。
     */
    const err_t posted = tcpip_callback(io_net_mdns_setup_cb, &ctx);
    if (posted != ERR_OK) {
        ESP_LOGE(TAG, "mDNS: tcpip_callback failed (%s)", lwip_strerr(posted));
        s_mdns_started = false;
        return ESP_FAIL;
    }
    /* tcpip_callback 只负责投递,不等待执行完;这里等它跑完再收结果 */
    for (int i = 0; i < 300 && !ctx.done; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (!ctx.done) {
        ESP_LOGE(TAG, "mDNS: setup callback timed out");
        s_mdns_started = false;
        return ESP_ERR_TIMEOUT;
    }
    if (ctx.rc != 0) {
        ESP_LOGE(TAG, "mDNS: setup failed (rc=%d)", ctx.rc);
        s_mdns_started = false;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "mDNS: %s.local  (service _ioagent._tcp.local:%u)",
             ctx.name, (unsigned)port);
    return ESP_OK;
#else
    ESP_LOGI(TAG, "MCP service reachable at http://%s:%u/  (mDNS responder not compiled in)",
             io_net_ip(), (unsigned)port);
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t io_net_disconnect(void)
{
    return esp_wifi_disconnect();
}
