#include "io_cfg.h"

#include <stdio.h>
#include <string.h>

#include <esp_log.h>
#include <esp_wifi.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <driver/uart.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "io_disk.h"
#include "io_led.h"
#include "io_log.h"

static const char *TAG = "io_cfg";

#define NVS_NS      "ioagent"   /* 绑定层:改名会读不到已存的 WiFi 配置 */
#define NVS_KEY_SSID "wifi_ssid"
#define NVS_KEY_PASS "wifi_pass"
#define NVS_KEY_PSK  "psk"
#define NVS_KEY_NAME "name"
#define NVS_KEY_LOG_IPS "log_ips"
#define NVS_KEY_PROV_CRC "prov_crc"

/* 配网文件:宿主写到虚拟盘上,设备自己取(见 io_cfg.h 的说明) */
#define PROV_FILE   "DEV.CFG"
#define PROV_MAX    512

static char s_name[IO_CFG_NAME_MAX] = "sophon";

/* ------------------------------------------------------------------ */
/* NVS 读写                                                            */
/* ------------------------------------------------------------------ */

static esp_err_t ns_get_str(const char *key, char *out, size_t len)
{
    nvs_handle_t h;
    esp_err_t rc = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (rc != ESP_OK) {
        return (rc == ESP_ERR_NVS_NOT_FOUND) ? ESP_ERR_NOT_FOUND : rc;
    }
    size_t out_len = len;
    rc = nvs_get_str(h, key, out, &out_len);
    nvs_close(h);
    return rc;
}

static esp_err_t ns_set_str(const char *key, const char *val)
{
    nvs_handle_t h;
    esp_err_t rc = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (rc != ESP_OK) {
        return rc;
    }
    rc = nvs_set_str(h, key, val);
    if (rc == ESP_OK) {
        rc = nvs_commit(h);
    }
    nvs_close(h);
    return rc;
}

static esp_err_t ns_get_u32(const char *key, uint32_t *out)
{
    nvs_handle_t h;
    esp_err_t rc = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (rc != ESP_OK) {
        return (rc == ESP_ERR_NVS_NOT_FOUND) ? ESP_ERR_NOT_FOUND : rc;
    }
    rc = nvs_get_u32(h, key, out);
    nvs_close(h);
    return rc;
}

static esp_err_t ns_set_u32(const char *key, uint32_t val)
{
    nvs_handle_t h;
    esp_err_t rc = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (rc != ESP_OK) {
        return rc;
    }
    rc = nvs_set_u32(h, key, val);
    if (rc == ESP_OK) {
        rc = nvs_commit(h);
    }
    nvs_close(h);
    return rc;
}

esp_err_t io_cfg_init(void)
{
    esp_err_t rc = nvs_flash_init();
    if (rc == ESP_ERR_NVS_NO_FREE_PAGES || rc == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs erase, reformatting");
        ESP_ERROR_CHECK(nvs_flash_erase());
        rc = nvs_flash_init();
    }
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init failed: %s", esp_err_to_name(rc));
        return rc;
    }

    char buf[IO_CFG_NAME_MAX];
    if (ns_get_str(NVS_KEY_NAME, buf, sizeof(buf)) == ESP_OK && buf[0]) {
        strncpy(s_name, buf, sizeof(s_name) - 1);
        s_name[sizeof(s_name) - 1] = '\0';
    }
    return ESP_OK;
}

esp_err_t io_cfg_get_wifi(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    char s[IO_CFG_WIFI_SSID_MAX];
    char p[IO_CFG_WIFI_PASS_MAX];

    esp_err_t rc = ns_get_str(NVS_KEY_SSID, s, sizeof(s));
    if (rc != ESP_OK) {
        return rc;
    }
    if (ns_get_str(NVS_KEY_PASS, p, sizeof(p)) != ESP_OK) {
        p[0] = '\0';
    }
    if (ssid && ssid_len) {
        strncpy(ssid, s, ssid_len - 1);
        ssid[ssid_len - 1] = '\0';
    }
    if (pass && pass_len) {
        strncpy(pass, p, pass_len - 1);
        pass[pass_len - 1] = '\0';
    }
    return ESP_OK;
}

esp_err_t io_cfg_set_wifi(const char *ssid, const char *pass)
{
    if (!ssid || !ssid[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t rc = ns_set_str(NVS_KEY_SSID, ssid);
    if (rc != ESP_OK) {
        return rc;
    }
    return ns_set_str(NVS_KEY_PASS, (pass && pass[0]) ? pass : "");
}

esp_err_t io_cfg_get_psk(char *psk, size_t len)
{
    return ns_get_str(NVS_KEY_PSK, psk, len);
}

esp_err_t io_cfg_set_psk(const char *psk)
{
    if (!psk || !psk[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    return ns_set_str(NVS_KEY_PSK, psk);
}

const char *io_cfg_device_name(void)
{
    return s_name;
}

/*
 * 日志服务的源 IP 授权表。
 * 空 = 没有授权表 = 日志功能关闭(设备端零开销,见 io_log.h)。
 */
esp_err_t io_cfg_get_log_ips(char *out, size_t len)
{
    if (!out || !len) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';
    return ns_get_str(NVS_KEY_LOG_IPS, out, len);
}

esp_err_t io_cfg_set_log_ips(const char *ips_csv)
{
    if (!ips_csv) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(ips_csv) >= IO_CFG_LOG_IPS_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    return ns_set_str(NVS_KEY_LOG_IPS, ips_csv);
}

/* ------------------------------------------------------------------ */
/* 配网文件(DEV.CFG):宿主写 U 盘 → 设备自己取                              */
/* ------------------------------------------------------------------ */

static uint32_t fnv1a(const uint8_t *d, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) {
        h ^= d[i];
        h *= 16777619u;
    }
    return h;
}

static void trim_ascii(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) {
        s[--n] = '\0';
    }
    size_t i = 0;
    while (s[i] == ' ' || s[i] == '\t') {
        i++;
    }
    if (i) {
        memmove(s, s + i, strlen(s + i) + 1);
    }
}

esp_err_t io_cfg_apply_provisioning_file(bool *changed)
{
    if (changed) {
        *changed = false;
    }

    char buf[PROV_MAX];
    size_t len = 0;
    if (io_disk_read_file(PROV_FILE, buf, sizeof(buf) - 1, &len) != ESP_OK || len == 0) {
        return ESP_ERR_NOT_FOUND;     /* 盘上没有配网文件 */
    }
    buf[len] = '\0';

    /* 幂等:同一份文件不重复应用(否则运行时改完 WiFi,盘上旧文件会改回去) */
    const uint32_t crc = fnv1a((const uint8_t *)buf, len);
    uint32_t old = 0;
    if (ns_get_u32(NVS_KEY_PROV_CRC, &old) == ESP_OK && old == crc) {
        return ESP_OK;
    }

    char ssid[IO_CFG_WIFI_SSID_MAX] = {0};
    char pass[IO_CFG_WIFI_PASS_MAX] = {0};
    char name[IO_CFG_NAME_MAX] = {0};
    char logips[IO_CFG_LOG_IPS_MAX] = {0};
    bool have_ssid = false;
    bool have_pass = false;
    bool have_logips = false;

    char *line = buf;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        if (nl) {
            *nl = '\0';
        }
        char *hash = strchr(line, '#');
        if (hash) {
            *hash = '\0';
        }
        trim_ascii(line);
        char *eq = strchr(line, '=');
        if (eq) {
            *eq = '\0';
            char *key = line;
            char *val = eq + 1;
            trim_ascii(key);
            trim_ascii(val);
            if (!strcmp(key, "ssid") && val[0]) {
                strncpy(ssid, val, sizeof(ssid) - 1);
                have_ssid = true;
            } else if (!strcmp(key, "pass")) {
                strncpy(pass, val, sizeof(pass) - 1);
                have_pass = true;    /* 写了 pass= 就是显式给密码(空=开放网络) */
            } else if (!strcmp(key, "name") && val[0]) {
                strncpy(name, val, sizeof(name) - 1);
            } else if (!strcmp(key, "logips")) {
                /*
                 * 日志服务的源 IP 授权表(逗号分隔)。
                 * 写空 = 清空授权表 = 关掉日志服务。
                 */
                strncpy(logips, val, sizeof(logips) - 1);
                have_logips = true;
            }
        }
        line = nl ? nl + 1 : NULL;
    }

    if (!have_ssid && !have_logips) {
        ESP_LOGW(TAG, "%s has no ssid and no logips -- ignored", PROV_FILE);
        return ESP_ERR_INVALID_ARG;
    }

    if (have_ssid) {
        /*
         * 没写 pass 键 = **沿用 NVS 里已有的密码**(只换 SSID 时不必重贴密码);
         * 写了 pass= (空) = 开放网络。
         */
        if (!have_pass) {
            char old[IO_CFG_WIFI_PASS_MAX] = {0};
            if (io_cfg_get_wifi(NULL, 0, old, sizeof(old)) == ESP_OK) {
                strncpy(pass, old, sizeof(pass) - 1);
                pass[sizeof(pass) - 1] = '\0';
            }
        }
        esp_err_t rc = io_cfg_set_wifi(ssid, pass);
        if (rc != ESP_OK) {
            return rc;
        }
        if (name[0]) {
            io_cfg_set_device_name(name);
        }
    }

    /*
     * 日志授权表:与 ssid 解耦 —— 只想改日志 IP 的话,DEV.CFG 里**只写
     * logips= 就行**,不必把 WiFi 密码再贴一遍。
     */
    if (have_logips) {
        esp_err_t rc = io_cfg_set_log_ips(logips);
        if (rc != ESP_OK) {
            return rc;
        }
    }

    esp_err_t rc = ns_set_u32(NVS_KEY_PROV_CRC, crc);
    if (rc != ESP_OK) {
        return rc;
    }
    /* 密码不打印 */
    ESP_LOGW(TAG, "provisioning applied from %s: ssid=\"%s\" pass=%s name=%s logips=%s",
             PROV_FILE, have_ssid ? ssid : "(unchanged)",
             have_ssid ? (pass[0] ? "<set>" : "<open>") : "-",
             have_ssid ? name : s_name,
             have_logips ? (logips[0] ? logips : "(cleared)") : "(unchanged)");
    if (changed) {
        *changed = true;
    }
    return ESP_OK;
}
esp_err_t io_cfg_set_device_name(const char *name)
{
    if (!name || !name[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t rc = ns_set_str(NVS_KEY_NAME, name);
    if (rc == ESP_OK) {
        strncpy(s_name, name, sizeof(s_name) - 1);
        s_name[sizeof(s_name) - 1] = '\0';
    }
    return rc;
}

/* ------------------------------------------------------------------ */
/* 串口命令行(首次配网)                                                */
/* ------------------------------------------------------------------ */

static void print_wifi_scan(void)
{
    wifi_scan_config_t cfg = {
        .show_hidden = false,
    };
    ESP_LOGI(TAG, "scanning for APs ...");
    ESP_ERROR_CHECK(esp_wifi_scan_start(&cfg, true));

    uint16_t n = 0;
    wifi_ap_record_t *ap = NULL;
    if (esp_wifi_scan_get_ap_num(&n) == ESP_OK && n > 0) {
        ap = (wifi_ap_record_t *)malloc(sizeof(wifi_ap_record_t) * n);
        if (ap) {
            if (esp_wifi_scan_get_ap_records(&n, ap) == ESP_OK) {
                for (uint16_t i = 0; i < n; i++) {
                    /* 用 printf 而不是 ESP_LOG:串口命令的输出要给人看,不带时间戳前缀 */
                    printf("ap: %s  ch=%d  rssi=%d\n", ap[i].ssid, ap[i].primary,
                           ap[i].rssi);
                }
            }
            free(ap);
        }
    }
    esp_wifi_scan_stop();
    printf("scan done, %u AP(s)\n", (unsigned)n);
}

static void print_status(void)
{
    char ssid[IO_CFG_WIFI_SSID_MAX];
    char pass[IO_CFG_WIFI_PASS_MAX];
    char psk[IO_CFG_PSK_MAX];

    if (io_cfg_get_wifi(ssid, sizeof(ssid), pass, sizeof(pass)) == ESP_OK) {
        printf("wifi: ssid=\"%s\" pass=%s\n", ssid, pass[0] ? "<set>" : "<open>");
    } else {
        printf("wifi: <not configured>\n");
    }
    if (io_cfg_get_psk(psk, sizeof(psk)) == ESP_OK) {
        printf("psk : <set>\n");
    } else {
        printf("psk : <not configured>\n");
    }
    printf("name: %s\n", s_name);
    char ips[IO_CFG_LOG_IPS_MAX] = {0};
    io_cfg_get_log_ips(ips, sizeof(ips));
    printf("log : allowlist=%s service=%s\n", ips[0] ? ips : "<none>",
           io_log_active() ? "on" : "off");
}

static void handle_line(char *line)
{
    /* 去掉行尾空白 */
    for (int i = (int)strlen(line) - 1; i >= 0 && (line[i] == '\r' || line[i] == '\n' || line[i] == ' '); i--) {
        line[i] = '\0';
    }
    if (!line[0]) {
        return;
    }
    printf("sophon> %s\n", line);

    if (strcmp(line, "wifi scan") == 0) {
        print_wifi_scan();
        return;
    }
    if (strcmp(line, "status") == 0) {
        print_status();
        return;
    }
    if (strncmp(line, "wifi set ", 9) == 0) {
        char *ssid = line + 9;
        char *pass = strchr(ssid, ' ');
        if (pass) {
            *pass++ = '\0';
        } else {
            pass = (char *)"";
        }
        esp_err_t rc = io_cfg_set_wifi(ssid, pass);
        printf("wifi set: %s\n", (rc == ESP_OK) ? "ok, reboot to apply" : esp_err_to_name(rc));
        return;
    }
    if (strncmp(line, "psk set ", 8) == 0) {
        esp_err_t rc = io_cfg_set_psk(line + 8);
        printf("psk set: %s\n", (rc == ESP_OK) ? "ok" : esp_err_to_name(rc));
        return;
    }
    if (strncmp(line, "name set ", 9) == 0) {
        esp_err_t rc = io_cfg_set_device_name(line + 9);
        printf("name set: %s\n", (rc == ESP_OK) ? "ok" : esp_err_to_name(rc));
        return;
    }
    /*
     * 日志服务的源 IP 授权表。
     * 主路径是 DEV.CFG 的 `logips=`(与 ssid 一并给用户),这条命令是
     * 应急/热切换通道 —— 开发机 DHCP 换了 IP 之后,不用重贴 U 盘。
     */
    if (strcmp(line, "led test") == 0) {
        /*
         * 灯自检:逐颗点亮(数灯珠)+ 全体依次红/绿/蓝/白(验颜色字节序)。
         * 灯珠数和字节序**取决于具体板子**,猜错就"该绿显示红"——
         * 让硬件自己把答案演出来,一次定死。
         */
        const esp_err_t rc = io_led_test();
        printf("led test: %s\n",
               (rc == ESP_OK) ? "running (~4s: pixels one by one, then all red/green/blue/white)"
                              : esp_err_to_name(rc));
        return;
    }
    if (strncmp(line, "log ", 4) == 0) {        char *arg = line + 4;
        char ips[IO_CFG_LOG_IPS_MAX] = {0};
        if (!strcmp(arg, "status")) {
            io_cfg_get_log_ips(ips, sizeof(ips));
            printf("log : allowlist=%s service=%s allowed=%u\n",
                   ips[0] ? ips : "<none>",
                   io_log_active() ? "on" : "off",
                   (unsigned)io_log_allow_count());
            return;
        }
        if (!strcmp(arg, "off")) {
            io_cfg_set_log_ips("");
            io_log_stop();
            printf("log : off (allowlist cleared)\n");
            return;
        }
        if (strncmp(arg, "ips ", 4) == 0) {
            char *csv = arg + 4;
            if (io_cfg_set_log_ips(csv) != ESP_OK) {
                printf("log ips: bad value (too long?)\n");
                return;
            }
            io_log_stop();          /* 已经在跑就先停干净 */
            if (io_log_net_start(csv) == ESP_OK) {
                io_log_net_open();
                printf("log ips: ok (%s)\n", csv);
            } else {
                printf("log ips: not started (bad ip list?)\n");
            }
            return;
        }
        printf("log: usage: log status | log ips <ip>[,<ip>...] | log off\n");
        return;
    }
    printf("unknown command (try: wifi scan / wifi set / psk set / name set / "
           "log ips / status)\n");
}

static void console_task(void *arg)
{
    (void)arg;
    static char line[128];
    size_t len = 0;

    printf("\n=== Sophon console ===\n");
    printf("commands: wifi scan | wifi set <ssid> [pass] | psk set <psk> | name set <n> | "
           "log ips <ip>[,<ip>] | log off | led test | status\n");
    printf("sophon> ");

    for (;;) {
        uint8_t ch;
        const int n = uart_read_bytes(UART_NUM_0, &ch, 1, portMAX_DELAY);
        if (n != 1) {
            continue;
        }
        if (ch == '\r' || ch == '\n') {
            if (len) {
                line[len] = '\0';
                handle_line(line);
                len = 0;
            }
            printf("sophon> ");
            continue;
        }
        if (ch == '\b' || ch == 127) {
            if (len) {
                len--;
            }
            continue;
        }
        if (ch >= 0x20 && len < sizeof(line) - 1) {
            line[len++] = (char)ch;
        }
    }
}

esp_err_t io_cfg_start_console(void)
{
    /* UART0 的 RX 也可能没接(只接 TX 做日志),读不到就安静待着。 */
    const uart_config_t cfg = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_APB,
    };
    if (uart_param_config(UART_NUM_0, &cfg) != ESP_OK) {
        ESP_LOGW(TAG, "console: UART0 param config failed, console disabled");
        return ESP_FAIL;
    }
    if (uart_driver_install(UART_NUM_0, 256, 0, 0, NULL, 0) != ESP_OK) {
        ESP_LOGW(TAG, "console: UART0 driver install failed, console disabled");
        return ESP_FAIL;
    }
    if (xTaskCreate(console_task, "cfgcon", 4096, NULL, 2, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "console task started on UART0");
    return ESP_OK;
}
