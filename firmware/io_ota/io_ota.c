/*
 * io_ota — 通过 WiFi 给设备刷整机固件(OTA)。详见 include/io_ota.h。
 */
#include "io_ota.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/time.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <esp_log.h>
#include <esp_system.h>
#include <esp_heap_caps.h>
#include <esp_ota_ops.h>
#include <esp_app_format.h>
#include <esp_http_client.h>

static const char *TAG = "io_ota";

#define IO_OTA_TASK_STACK  6144
#define IO_OTA_TASK_PRIO    3
#define IO_OTA_CHUNK        4096
/* 刷完之后等一会儿再重启:让 MCP 的应答先发出去(否则 agent 看到的
 * 是"连接被重置",分不清是刷成功还是设备挂了) */
#define IO_OTA_REBOOT_DELAY_MS 2500
/* 动手之前也等一会儿:esp_ota_begin 擦除目标槽期间 flash 独占,
 * httpd 在那段时间**发不出应答**(取指都得等 flash)。不先歇一下,
 * 发 flash 的那条 MCP 连接会超时,agent 以为失败,其实已经在刷了。 */
#define IO_OTA_ACK_DELAY_MS  1500

static volatile bool s_busy;
static io_ota_info_t s_info = {
    .state = "idle", .url = {0}, .received = 0, .total = -1, .error = {0},
};

/*
 * 分区标签**缓存**(理由见 refresh_partition_labels 处的注释)。
 */
static char s_boot_label[18];
static char s_next_label[18];
static bool s_labels_ok;

static void ota_set_state(const char *st)
{
    snprintf(s_info.state, sizeof(s_info.state), "%s", st);
}

static void ota_fail(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_info.error, sizeof(s_info.error), fmt, ap);
    va_end(ap);
    ota_set_state("failed");
    s_busy = false;
    ESP_LOGE(TAG, "OTA failed: %s", s_info.error);
}

static void ota_task(void *arg)
{
    (void)arg;
    char *url = strdup(s_info.url);
    if (!url) {
        ota_fail("out of memory");
        vTaskDelete(NULL);
        return;
    }

    /* 等 MCP 应答先落地(见 IO_OTA_ACK_DELAY_MS 的说明) */
    vTaskDelay(pdMS_TO_TICKS(IO_OTA_ACK_DELAY_MS));

    ESP_LOGI(TAG, "downloading %s", url);

    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (!target) {
        ota_fail("no OTA slot available (partition table?)");
        free(url);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "target slot: %s @ 0x%x, size %u",
             target->label, (unsigned)target->address, (unsigned)target->size);

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 15000,
        .keep_alive_enable = false,
        .disable_auto_redirect = false,
        /* 收包缓冲区。默认 1KB 对 1MB 镜像意味着上千次读,提到 4KB。 */
        .buffer_size = IO_OTA_CHUNK,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ota_fail("http client init failed");
        free(url);
        vTaskDelete(NULL);
        return;
    }
    esp_http_client_set_method(client, HTTP_METHOD_GET);
    esp_http_client_set_header(client, "Connection", "close");

    /*
     * 注意:这里**不能**用 esp_http_client_perform()。
     * perform() 内部会把 response body 整个从 socket 抽干并且不缓存
     * (esp_http_client.c: perform 里 cache_data_in_fetch_hdr=0,
     *  接着 get_data 循环把 content_length 全部读走)。
     * 等 perform() 返回后再调 esp_http_client_read() 时,
     * data_process 已经等于 content_length ⇒ is_data_remain 为假
     * ⇒ 第一次 read() 就返回 0(表现:OTA 收到 0 字节,ota_end 报镜像无效)。
     * 所以走原生流式路径: open → fetch_headers → read。
     */
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ota_fail("http open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        free(url);
        vTaskDelete(NULL);
        return;
    }
    if (esp_http_client_fetch_headers(client) < 0) {
        ota_fail("fetch headers failed");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        free(url);
        vTaskDelete(NULL);
        return;
    }
    const int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        ota_fail("http status %d", status);
        esp_http_client_cleanup(client);
        free(url);
        vTaskDelete(NULL);
        return;
    }
    s_info.total = (int32_t)esp_http_client_get_content_length(client);
    ESP_LOGI(TAG, "content-length: %d bytes", s_info.total);

    ota_set_state("downloading");

    /*
     * 两条路径,取决于镜像能不能整个塞进 PSRAM:
     *
     *  A. 塞得下 ⇒ **先把整份镜像下到 PSRAM**,下完之后才动 flash。
     *     这段时间(下载是最长的一段,十几秒)设备完全健康:
     *     status 能查进度,别的工具也照常работа。
     *     flash 独占只发生在最后:擦除 + 写入 + 校验,几秒。
     *
     *  B. 塞不下(镜像比分区大 / PSRAM 分配失败 / 服务器不给 Content-Length)
     *     ⇒ 退回边下边写。功能一样,但整个过程设备都是半死的。
     */
    uint8_t *img = NULL;
    if (s_info.total > 0 && (size_t)s_info.total <= (size_t)target->size) {
        img = heap_caps_malloc((size_t)s_info.total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (img) {
            ESP_LOGI(TAG, "buffering %d bytes in PSRAM (device stays responsive while downloading)",
                     s_info.total);
        } else {
            ESP_LOGW(TAG, "no PSRAM buffer; falling back to streaming write");
        }
    }

    esp_ota_handle_t handle = 0;
    int64_t total = 0;

    if (img) {
        /* ---- 路径 A:先下到 PSRAM ---- */
        while (total < s_info.total) {
            int want = IO_OTA_CHUNK;
            if ((int64_t)want > (int64_t)s_info.total - total) {
                want = (int)(s_info.total - total);
            }
            const int n = esp_http_client_read(client, (char *)img + total, want);
            if (n < 0) {
                ota_fail("http read error %d", n);
                goto done;
            }
            if (n == 0) {
                ota_fail("short read: got %lld of %d bytes", (long long)total, s_info.total);
                goto done;
            }
            total += n;
            s_info.received = (int32_t)total;
            if ((total % (256 * 1024)) < IO_OTA_CHUNK) {
                ESP_LOGI(TAG, "progress: %lld / %d bytes", (long long)total, s_info.total);
            }
        }
        ESP_LOGI(TAG, "download complete (%lld bytes); now touching flash", (long long)total);

        /* 到这里才动 flash —— 设备会短暂失联几秒 */
        err = esp_ota_begin(target, OTA_SIZE_UNKNOWN, &handle);
        if (err != ESP_OK) {
            ota_fail("ota_begin failed: %s", esp_err_to_name(err));
            goto done;
        }
        if (esp_ota_write(handle, img, (size_t)total) != ESP_OK) {
            ota_fail("ota_write failed");
            esp_ota_abort(handle);
            goto done;
        }
        free(img);
        img = NULL;
    } else {
        /* ---- 路径 B:边下边写 ---- */
        err = esp_ota_begin(target, OTA_SIZE_UNKNOWN, &handle);
        if (err != ESP_OK) {
            ota_fail("ota_begin failed: %s", esp_err_to_name(err));
            goto done;
        }
        char *buf = malloc(IO_OTA_CHUNK);
        if (!buf) {
            ota_fail("out of memory");
            esp_ota_abort(handle);
            goto done;
        }
        while (1) {
            const int n = esp_http_client_read(client, buf, IO_OTA_CHUNK);
            if (n < 0) {
                ota_fail("http read error %d", n);
                free(buf);
                esp_ota_abort(handle);
                goto done;
            }
            if (n == 0) {
                break;                  /* 读完了 */
            }
            if (esp_ota_write(handle, buf, n) != ESP_OK) {
                ota_fail("ota_write failed at %lld bytes", (long long)total);
                free(buf);
                esp_ota_abort(handle);
                goto done;
            }
            total += n;
            s_info.received = (int32_t)total;
            if ((total % (256 * 1024)) < IO_OTA_CHUNK) {
                ESP_LOGI(TAG, "progress: %lld / %d bytes", (long long)total, s_info.total);
            }
        }
        free(buf);
        /* 长度核对: content_length 已知时必须收全,否则宁可不刷(不切启动分区) */
        if (s_info.total > 0 && total != s_info.total) {
            ota_fail("short read: got %lld of %d bytes", (long long)total, s_info.total);
            esp_ota_abort(handle);
            goto done;
        }
    }

    /* 结束并校验(描述符 + SHA256)。失败 ⇒ 当前分区不动,不会变砖 */
    err = esp_ota_end(handle);
    if (err != ESP_OK) {
        ota_fail("ota_end (image invalid?) failed: %s", esp_err_to_name(err));
        goto done;
    }
    ESP_LOGI(TAG, "image written and verified: %lld bytes", (long long)total);

    /* 切启动分区 */
    err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK) {
        ota_fail("set_boot_partition failed: %s", esp_err_to_name(err));
        goto done;
    }
    s_labels_ok = false;      /* 标签变了,下次 status 重算 */
    ota_set_state("staged");
    s_busy = false;
    ESP_LOGI(TAG, "staged in %s, rebooting in %d ms", target->label, IO_OTA_REBOOT_DELAY_MS);
    vTaskDelay(pdMS_TO_TICKS(IO_OTA_REBOOT_DELAY_MS));
    ota_set_state("rebooting");
    esp_restart();

done:
    free(img);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    free(url);
    vTaskDelete(NULL);
}

esp_err_t io_ota_start(const char *url)
{
    if (!url || !url[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_busy) {
        return ESP_ERR_INVALID_STATE;
    }
    /* 只接受 http/https(本项目开发机起的是明文 http) */
    if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(url) >= sizeof(s_info.url)) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(&s_info, 0, sizeof(s_info));
    ota_set_state("downloading");
    snprintf(s_info.url, sizeof(s_info.url), "%s", url);
    s_info.total = -1;
    s_busy = true;

    if (xTaskCreate(ota_task, "io_ota", IO_OTA_TASK_STACK, NULL,
                    IO_OTA_TASK_PRIO, NULL) != pdPASS) {
        s_busy = false;
        ota_set_state("failed");
        snprintf(s_info.error, sizeof(s_info.error), "task create failed");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool io_ota_busy(void)
{
    return s_busy;
}

/*
 * 分区标签**缓存**。
 *
 * 为什么不能每次现算:`esp_ota_get_boot_partition()` 内部会
 * `esp_partition_mmap()` 整个 otadata 分区(8 KB),要**连续的内部 RAM 虚拟地址**。
 * status 是高频调用,每次都 mmap 一次 ⇒ 内存一紧就
 * `mmap: esp_mmu_map(551): no mem`,而 status 失败会连带
 * lwIP 分配失败 → `httpd_sock_err: error in send : 11` → 连接被掐断。
 * (真机踩过:并发打设备时 status/read 间歇性失败,根因就是它。)
 *
 * 分区标签只会在 OTA 切换启动分区时变 ⇒ 算一次缓存住,切完再作废。
 */
static void refresh_partition_labels(void)
{
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    if (boot && next) {
        snprintf(s_boot_label, sizeof(s_boot_label), "%s", boot->label);
        snprintf(s_next_label, sizeof(s_next_label), "%s", next->label);
        s_labels_ok = true;
    }
}

esp_err_t io_ota_get_info(io_ota_info_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = s_info;
    /*
     * 带上分区标签:刷完固件后 boot 会从 ota_0 变成 ota_1 —— 这是
     * "OTA 真的写进去并切了启动分区"最直接的凭据(比版本号可靠,
     * 因为刷同一份构建时版本号不变)。
     */
    if (!s_labels_ok) {
        refresh_partition_labels();
    }
    if (s_labels_ok) {
        snprintf(out->boot, sizeof(out->boot), "%s", s_boot_label);
        snprintf(out->next, sizeof(out->next), "%s", s_next_label);
    }
    return ESP_OK;
}

esp_err_t io_ota_get_build(io_ota_build_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    const esp_app_desc_t *d = esp_app_get_description();
    if (!d) {
        return ESP_FAIL;
    }
    snprintf(out->version, sizeof(out->version), "%s", d->version);
    snprintf(out->idf_ver, sizeof(out->idf_ver), "%s", d->idf_ver);
    snprintf(out->project, sizeof(out->project), "%s", d->project_name);
    /*
     * esp_app_desc_t 的 date/time 是**字符串**(esptool 填的):
     *   date = "Oct 25 2026 08:43:00" 之类的可读时间戳
     *   time = "08:43:00"
     * 直接用 date 就行,别自己拼 —— 拼的格式不对反而误导。
     */
    snprintf(out->build, sizeof(out->build), "%s", d->date);
    return ESP_OK;
}
