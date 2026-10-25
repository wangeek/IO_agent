/*
 * io_mcp — Sophon 设备端 MCP server(Streamable HTTP + JSON-RPC 2.0)
 *
 * 拓扑:设备是服务端,agent 主动连过来(mDNS: _ioagent._tcp.local)。
 *
 * 端点:
 *   POST /mcp         JSON-RPC 2.0(MCP Streamable HTTP)
 *   GET  /mcp         405(我们不开 SSE 流,规范允许这样声明)
 *   GET  /files/<name> 共享盘上某个文件的原始字节
 *   GET  /ping        HTTP 存活探针
 *
 * 工具(全部是动词,全小写单词,零分隔符):
 *   status    设备身份与状态(含 watcher 是否在跑)
 *   ls        列共享盘
 *   read      读共享盘上某个文件
 *   capture   取最新一帧(按 FAT16 最后写入时间判)
 *   input     注入一个动作(键盘/鼠标/宏)
 *   update    把内容写进 flash 槽位(下次开机生效)
 *
 * ★ 安全语义(2026-10-22 事故后的定案)★
 *   - `input` **单次执行、零重试**:不因为失败就自动重发。
 *   - 注入前后各取一帧由 agent 自己用 capture 做 —— **由 agent 判断
 *     "指令有没有生效"**,而不是靠日志里的"已发送"。
 *   - 开机自动拉起默认**关闭**(见 io_launcher/CMakeLists.txt)。
 */
#include "io_mcp.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>

#include <esp_log.h>
#include <esp_http_server.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_check.h>
#include <cJSON.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

#include "io_cfg.h"
#include "io_disk.h"
#include "io_hid.h"
#include "io_launcher.h"
#include "io_led.h"
#include "io_net.h"
#include "io_log.h"
#include "io_ota.h"
#include "io_payload.h"
#include "io_usb.h"

static const char *TAG = "io_mcp";

#define IO_MCP_PORT          8080
#define IO_MCP_PROTO         "2025-06-18"
#define IO_MCP_SERVER_NAME   "sophon"
#define IO_MCP_SERVER_VER    "1.0.0"

/*
 * body 大小上限。
 *
 * 上限怎么定:最大的合法请求是 `update` —— 把内容 base64 装进
 * AGENT.EXE 槽(64 KB 原字节 → base64 约 87 KB),所以 128 KB 够用还有富余。
 * 上限的意义不是"防大包"而是**不让 content_len 变成无界的 malloc**:
 * httpd 只有一个 task,一个超大 body 会把整个服务打死(读 body 的那一步
 * 是在 httpd task 里同步做的)。
 */
#define IO_MCP_MAX_BODY      (128 * 1024)

/*
 * body 超限时"抽干"的上限:读掉多少才放弃并回 413。
 * 抽干是为了让客户端能干净地读到 413(直接关连接的话客户端只会看到 RST),
 * 但不能无限读 —— 所以设个上限。
 */
#define IO_MCP_MAX_DRAIN     (256 * 1024)

/*
 * 支持的 MCP 协议版本(协商用)。客户端 initialize 时报自己要的版本,
 * 我们支持就原样回,不支持就回最新的那个,由客户端决定要不要继续。
 */
static const char *const IO_MCP_PROTO_SUPPORTED[] = {
    "2024-11-05",
    "2025-03-26",
    "2025-06-18",
};

/* ------------------------------------------------------------------ */
/* 共享盘读文件                                                          */
/* ------------------------------------------------------------------ */
/*
 * 这个设备对外只有两件事:**读共享盘**、**往目标机送输入**。
 * 盘是目标机可见的那张 FAT16 卷(watcher 把帧写在上面),agent 通过 mDNS
 * 找到设备后直接读。所以文件读出来有两种去处:
 *   - 够小 → 直接内联进 MCP 结果(文本块或 image 块)
 *   - 太大 → 只给 URL,客户端自己去 GET /files/<name>
 */

/*
 * 内联进 MCP 结果的大小上限(超过就只给 URL)。
 *
 * 64 KB:base64 后约 87 KB。**能一次发完** —— lwIP 的发送缓冲
 * (SO_SNDBUF = 2×TCP_SND_BUF)已调到 32 KB,配合 /files/ 的分块流式,
 * 大内容走 URL 那条路 ⇒ 这条通道上不会撞上"发不出去"。
 * ★ 不要为了"看起来小"就砍这个阈值 ★ 砍了会让 capture 的 image 块
 * 消失(帧几乎都超过 8 KB)⇒ agent 拿不到图,那是契约降级。
 */
#define IO_FILE_MAX_INLINE   (64 * 1024)
/* 列目录一次最多看多少项(超过就截断并报 count)。 */
#define IO_DIR_MAX            64
/*
 * 读一个文件时的缓冲(静态,httpd 单 task 用)。
 *
 * ★ 16 KB,不是 64 KB ★
 *   这是**静态**缓冲,占的是内部 RAM(设备端只有 512 KB)。原来 64 KB,
 *   挤得 lwIP 分配不到内存 ⇒ `mmap: esp_mmu_map(551): no mem` +
 *   `httpd_sock_err: error in send : 11` ⇒ 并发打设备时 MCP 侧间歇性
 *   连接失败(真机踩过)。大文件靠 `io_disk_read_file_at` 分块读,不需要大缓冲。
 */
#define IO_FILE_BUF_MAX      (16 * 1024)

/* watcher 写的帧文件(目标机上的脚本产出)。 */
static bool name_is_frame(const char *name)
{
    static const char *const frames[] = { "SNAP_P.JPG", "SNAP_A.JPG", "SNAP_R.JPG" };
    for (size_t i = 0; i < sizeof(frames) / sizeof(frames[0]); i++) {
        if (!strcmp(name, frames[i])) {
            return true;
        }
    }
    return false;
}

/* 原始字节通道的 URL(大文件/非 MCP 客户端走这里)。 */
static char *files_url(const char *name)
{
    char *buf = (char *)malloc(96);
    if (buf) {
        snprintf(buf, 96, "http://%s:%d/files/%s", io_net_ip(), IO_MCP_PORT, name);
    }
    return buf;
}

/* JPEG 魔数(用来判断读出来的是不是图)。 */
static bool bytes_are_jpeg(const uint8_t *p, size_t len)
{
    return len >= 3 && p[0] == 0xFF && p[1] == 0xD8 && p[2] == 0xFF;
}

/* ------------------------------------------------------------------ */
/* 工具结果                                                            */
/* ------------------------------------------------------------------ */
/*
 * MCP 有两条错误通道(规范 "Error Handling"):
 *   - **协议错误**   → JSON-RPC error(未知工具、参数非法)
 *   - **工具执行错误** → 结果里 isError:true(API 失败、输入不合法、业务错误)
 *
 * 之前只有 input / update 设 isError,读类工具的失败只是在内层 JSON 里
 * 给 ok:false —— **合规的客户端会把失败当成功**。所以每个工具都返回这个结构,
 * 由 tool_result() 统一决定 isError。
 */
typedef struct {
    cJSON      *payload;    /**< 结构化结果(同时序列化进 TextContent) */
    bool        is_error;   /**< 工具执行错误 → isError:true */
    bool        has_image;  /**< payload 里的 image_b64 要作为 image 内容块发出 */
    const char *mime;       /**< 图片类型 */
} io_tool_out_t;

/* 造一个工具结果。 */
static io_tool_out_t tool_out(cJSON *payload, bool is_error)
{
    io_tool_out_t o = { payload, is_error, false, NULL };
    return o;
}

/* 把结构化结果包成 MCP 的 CallToolResult。 */static cJSON *tool_result(const io_tool_out_t *out)
{
    cJSON *result = cJSON_CreateObject();
    cJSON *content = cJSON_CreateArray();

    /* 图片内容块 —— MCP 原生的二进制通道(base64 内联,客户端/视觉模型直接能用) */
    char *b64 = NULL;
    if (out->has_image) {
        const cJSON *v = cJSON_GetObjectItem(out->payload, "image_b64");
        if (cJSON_IsString(v)) {
            b64 = (char *)malloc(strlen(v->valuestring) + 1);
            if (b64) {
                strcpy(b64, v->valuestring);
            }
        }
    }

    /* 文本块:结构化结果的序列化副本(规范:有结构化内容时也应给一份文本) */
    {
        cJSON *copy = cJSON_Duplicate(out->payload, true);
        cJSON_DeleteItemFromObject(copy, "image_b64");
        char *txt = cJSON_PrintUnformatted(copy);
        cJSON_Delete(copy);

        cJSON *t = cJSON_CreateObject();
        cJSON_AddStringToObject(t, "type", "text");
        cJSON_AddStringToObject(t, "text", txt ? txt : "{}");
        free(txt);
        cJSON_AddItemToArray(content, t);
    }

    if (b64) {
        cJSON *img = cJSON_CreateObject();
        cJSON_AddStringToObject(img, "type", "image");
        cJSON_AddStringToObject(img, "data", b64);
        cJSON_AddStringToObject(img, "mimeType", out->mime ? out->mime : "image/jpeg");
        cJSON_AddItemToArray(content, img);
        free(b64);
    }

    cJSON_AddItemToObject(result, "content", content);
    if (out->is_error) {
        cJSON_AddBoolToObject(result, "isError", true);
    }
    /* 结构化内容(规范里的 structuredContent) */
    {
        cJSON *sc = cJSON_Duplicate(out->payload, true);
        cJSON_DeleteItemFromObject(sc, "image_b64");
        cJSON_AddItemToObject(result, "structuredContent", sc);
    }
    return result;
}

/* ------------------------------------------------------------------ */
/* 单例执行槽 + worker task                                            */
/* ------------------------------------------------------------------ */
/*
 * 设备端是**单例**:同一时刻只跑一个动作。有动作在执行时,再来一个
 * 动作请求 → **直接回 busy**,不排队执行。
 *
 * 为什么长动作必须移出 httpd task:esp_http_server 是**单 task**
 * (一个 server task 串行处理所有会话)—— 长动作跑在 handler 里的话,
 * 第二个请求连 handler 都进不去,物理上拿不到 busy。挪到 worker task
 * 之后,httpd task 一直能应答,busy 才问得出来。
 *
 * 好处:注入进行中,**读类工具(ls / read / capture / status)
 * 照常服务** —— 注入时"眼"是睁着的。
 *
 * 不引入 job 状态机/轮询端点:**判据本来就是盘上的标记文件**
 * (INJ.TXT / CAP.TXT),agent 直接回读即可。
 */
#define IO_JOB_TEXT_MAX      256
#define IO_JOB_KIND_TEXT     1
#define IO_JOB_KIND_MACRO    2
/* type 超过这个长度就算长动作(按 60ms/字算,约 1s) */
#define IO_JOB_TYPE_ASYNC_CHARS 16

typedef struct {
    int  kind;
    bool ime_toggle;               /* 宏:是否先盲切一次输入法态 */
    char text[IO_JOB_TEXT_MAX];
} io_job_t;

static QueueHandle_t  s_job_q;
/* 单例槽用**二值信号量**而不是 mutex:槽会跨任务交接(httpd task 取,
 * worker task 还),mutex 有"必须由持有者释放"的限制,二值信号量没有。 */
static SemaphoreHandle_t s_op_sem;
static volatile bool   s_op_busy;    /* 仅用于对外报告状态 */

/* 抢单例槽:抢到返回 true,忙则 false(不等待、不排队)。 */
static bool op_slot_try(void)
{
    if (!s_op_sem || xSemaphoreTake(s_op_sem, 0) != pdTRUE) {
        return false;
    }
    s_op_busy = true;
    io_led_busy_acquire();
    return true;
}

static void op_slot_release(void)
{
    s_op_busy = false;
    io_led_busy_release();
    if (s_op_sem) {
        xSemaphoreGive(s_op_sem);
    }
}

static void io_job_task(void *arg)
{
    io_job_t job;
    (void)arg;
    for (;;) {
        if (xQueueReceive(s_job_q, &job, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        switch (job.kind) {
        case IO_JOB_KIND_TEXT:
            (void)io_hid_type_text(job.text, IO_HID_KEY_HOLD_MS);
            break;
        case IO_JOB_KIND_MACRO:
            if (!strcmp(job.text, "selftest")) {
                (void)io_launcher_run_selftest();
            } else if (!strcmp(job.text, "launch_watcher")) {
                (void)io_launcher_run_launch(job.ime_toggle);
            } else if (!strcmp(job.text, "ime_toggle")) {
                (void)io_launcher_ime_toggle();
            } else if (!strcmp(job.text, "focus_reset")) {
                (void)io_launcher_focus_reset();
            } else {
                ESP_LOGW(TAG, "unknown macro: %s", job.text);
            }
            break;
        default:
            break;
        }
        op_slot_release();    /* 释放单例槽 */
    }
}

/* ------------------------------------------------------------------ */
/* 小工具                                                               */
/* ------------------------------------------------------------------ */

static const char *s_b64_alphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int b64_val(char c)
{
    if (c >= 'A' && c <= 'Z') { return c - 'A'; }
    if (c >= 'a' && c <= 'z') { return c - 'a' + 26; }
    if (c >= '0' && c <= '9') { return c - '0' + 52; }
    if (c == '+') { return 62; }
    if (c == '/') { return 63; }
    return -1;
}

/* 字节 -> base64 字符串(调用方 free)。给 MCP 的 image 内容块用。 */
static char *b64_encode(const uint8_t *in, size_t len)
{
    const size_t out_len = ((len + 2) / 3) * 4;
    char *out = (char *)malloc(out_len + 1);
    if (!out) {
        return NULL;
    }
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        const uint32_t b0 = in[i];
        const uint32_t b1 = (i + 1 < len) ? in[i + 1] : 0;
        const uint32_t b2 = (i + 2 < len) ? in[i + 2] : 0;
        const uint32_t v = (b0 << 16) | (b1 << 8) | b2;
        out[o++] = s_b64_alphabet[(v >> 18) & 0x3F];
        out[o++] = s_b64_alphabet[(v >> 12) & 0x3F];
        out[o++] = (i + 1 < len) ? s_b64_alphabet[(v >> 6) & 0x3F] : '=';
        out[o++] = (i + 2 < len) ? s_b64_alphabet[v & 0x3F] : '=';
    }
    out[o] = '\0';
    return out;
}

/* base64 -> 字节。返回解码后的长度,失败返回 -1。 */
static int b64_decode(const char *in, uint8_t *out, size_t out_max)
{
    int acc = 0, bits = 0;
    size_t n = 0;
    for (const char *p = in; *p && *p != '='; p++) {
        const int v = b64_val(*p);
        if (v < 0) {
            continue;   /* 忽略空白 */
        }
        acc = (acc << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n >= out_max) {
                return -1;
            }
            out[n++] = (uint8_t)((acc >> bits) & 0xFF);
        }
    }
    return (int)n;
}

/* ------------------------------------------------------------------ */
/* JSON 辅助                                                            */
/* ------------------------------------------------------------------ */

/* JSON-RPC 的两种应答:错误 / 成功。定义在文件后部,这里先声明。 */
static cJSON *rpc_error(const cJSON *id, int code, const char *msg);
static cJSON *rpc_ok(const cJSON *id, cJSON *result);

/*
 * 从共享盘读一个文件。
 *
 * buf 必须是静态缓冲(httpd 是单 task,静态缓冲不会有并发问题;
 * 栈上放不下 16 KB)。
 *
 * @return 读到的字节数;失败返回 0
 */
static size_t read_shared_file(const char *name, uint8_t *buf, size_t max)
{
    size_t len = 0;
    const int64_t t0 = esp_timer_get_time();
    if (io_disk_read_file(name, buf, max, &len) != ESP_OK || len == 0) {
        return 0;
    }
    const int64_t dt_ms = (esp_timer_get_time() - t0) / 1000;
    /*
     * 慢读打点:读盘是从 FAT 簇链上拷到 PSRAM,正常是毫秒级。
     * 超过 500 ms 就说明有异常(簇链走得太远、或内存被换出),
     * 打出来才能在串口日志里对上"读盘卡住"这类现象。
     */
    if (dt_ms > 500) {
        ESP_LOGW(TAG, "read %s took %lld ms", name, (long long)dt_ms);
    }
    return len;
}


/* ------------------------------------------------------------------ */
/* 工具:status                                                         */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* 设备状态码(LED 语义)                                                 */
/* ------------------------------------------------------------------ */
/*
 * 一个 int,不多不少。语义表(见 README 决策表):
 *   0 就绪      —— 已联网 + MCP 在服务
 *   1 启动中    —— 刚上电,还没开始连网
 *   2 网络未就绪 —— 在连/没配网/掉线重连
 *   3 动作执行中 —— 收到 act,执行期间持续亮
 *   4 故障      —— 上次注入自检失败("手不灵")
 *
 * 原则:只放**设备自己能测出来**、且**现场的人会据此行动**的状态;
 * 颜色与节奏的对应只写在文档里,JSON 里不带颜色/文案/子状态。
 *
 * 状态的**计算**住在 io_led(见 io_led.h):LED 是物理输出,
 * 没人调 status 的时候它也得自己亮 ⇒ 算状态不能挂在 status 请求上。
 * 这里只是把同一个值报给 agent。
 */


static io_tool_out_t tool_status(void)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddStringToObject(o, "device", io_cfg_device_name());
    cJSON_AddStringToObject(o, "fw", "m2-remote");
    cJSON_AddStringToObject(o, "ip", io_net_ip());
    cJSON_AddBoolToObject(o, "wifi_connected", io_net_is_connected());
    cJSON_AddNumberToObject(o, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddNumberToObject(o, "sectors", (double)io_disk_sector_count());

    cJSON *link = cJSON_CreateObject();
    cJSON_AddBoolToObject(link, "mounted", io_usb_mounted());
    cJSON_AddBoolToObject(link, "suspended", io_usb_suspended());
    cJSON_AddBoolToObject(link, "host_configured", io_usb_host_configured());
    cJSON_AddBoolToObject(link, "link_alive", io_usb_link_alive());
    cJSON_AddItemToObject(o, "usb", link);

    cJSON *hid = cJSON_CreateObject();
    cJSON_AddBoolToObject(hid, "endpoint_ready", io_hid_ready());
    cJSON_AddBoolToObject(hid, "layout_ok", io_hid_layout_selfcheck());
    cJSON_AddItemToObject(o, "hid", hid);

    /* 单例槽状态:agent 轮询它就知道后台动作跑完没有 */
    cJSON_AddBoolToObject(o, "busy", s_op_busy);

    /* 设备状态码:一个 int(语义见 README 决策表"LED 语义"那一行) */
    cJSON_AddNumberToObject(o, "led", io_led_state());

    uint32_t count = 0;
    const io_slot_desc_t *tbl = io_disk_slot_table(&count);
    cJSON *slots = cJSON_CreateArray();
    for (uint32_t i = 0; i < count; i++) {
        cJSON *s = cJSON_CreateObject();
        cJSON_AddStringToObject(s, "name", tbl[i].name);
        cJSON_AddStringToObject(s, "kind",
                                (tbl[i].kind == IO_SLOT_KIND_INBOX) ? "inbox" : "slot");
        cJSON_AddNumberToObject(s, "capacity", (double)tbl[i].capacity);
        cJSON_AddNumberToObject(s, "len", (double)io_payload_active_len((uint32_t)i));
        cJSON_AddItemToArray(slots, s);
    }
    cJSON_AddItemToObject(o, "slots", slots);
    cJSON_AddNumberToObject(o, "payload_seq", (double)io_payload_active_seq());

    /*
     * watcher 在不在目标机上跑 —— 判据就是盘上有没有 CAP.TXT。
     * (以前这是一个独立工具 watcher/status,现在并进 status:
     *  设备只有一个"状态"入口,拆成两个反而让 agent 不知道该问哪个。)
     */
    {
        io_file_info_t cap;
        const bool running = io_disk_find_file("CAP.TXT", &cap);
        cJSON *w = cJSON_CreateObject();
        cJSON_AddBoolToObject(w, "running", running);
        if (running) {
            cJSON_AddNumberToObject(w, "cap_mtime", (double)cap.mtime);
        }
        cJSON_AddItemToObject(o, "watcher", w);
    }

    /*
     * 网络日志通道的状态。
     * 单 Type-C 口的 dongle 没有串口了 ⇒ 日志只能走 WiFi,
     * 所以"日志通道通没通"必须能被 agent 直接看见,而不是靠猜。
     *   active  : 服务是否在跑(没配 IP 授权表就是 false ⇒ 日志面为零)
     *   allowed : 授权表里的 IP 个数
     *   rx      : 收到过的 UDP 包数
     *   dropped : 其中因为源 IP 不在授权表被丢掉的(有人在问但没授权)
     *   tx      : 回出去的包数 / 字节数
     */
    {
        cJSON *lg = cJSON_CreateObject();
        cJSON_AddNumberToObject(lg, "port", (double)io_log_port());
        cJSON_AddBoolToObject(lg, "active", io_log_active());
        cJSON_AddBoolToObject(lg, "open", io_log_opened());
        cJSON_AddNumberToObject(lg, "allowed", (double)io_log_allow_count());
        cJSON_AddNumberToObject(lg, "rx", (double)io_log_rx_packets());
        cJSON_AddNumberToObject(lg, "dropped", (double)io_log_rx_rejected());
        cJSON_AddNumberToObject(lg, "tx", (double)io_log_tx_packets());
        cJSON_AddNumberToObject(lg, "bytes", (double)io_log_tx_bytes());
        cJSON_AddNumberToObject(lg, "hook", (double)io_log_hook_calls());
        cJSON_AddItemToObject(o, "log", lg);
    }

    /*
     * OTA 状态 + 当前固件身份。
     * 刷完固件后 agent 靠 `app.build` 确认"设备上的固件真的换了"
     * —— 比看 uptime 可靠。
     */
    {
        io_ota_info_t oi;
        cJSON *ot = cJSON_CreateObject();
        if (io_ota_get_info(&oi) == ESP_OK) {
            cJSON_AddStringToObject(ot, "state", oi.state);
            cJSON_AddNumberToObject(ot, "received", (double)oi.received);
            cJSON_AddNumberToObject(ot, "total", (double)oi.total);
            if (oi.boot[0]) {
                cJSON_AddStringToObject(ot, "boot", oi.boot);
                cJSON_AddStringToObject(ot, "next", oi.next);
            }
            if (oi.error[0]) {
                cJSON_AddStringToObject(ot, "error", oi.error);
            }
        }
        cJSON_AddItemToObject(o, "ota", ot);

        io_ota_build_t bd;
        cJSON *ab = cJSON_CreateObject();
        if (io_ota_get_build(&bd) == ESP_OK) {
            cJSON_AddStringToObject(ab, "version", bd.version);
            cJSON_AddStringToObject(ab, "idf", bd.idf_ver);
            cJSON_AddStringToObject(ab, "build", bd.build);
        }
        cJSON_AddItemToObject(o, "app", ab);
    }

    io_tool_out_t out = { o, false, false, NULL };
    return out;
}

/* ------------------------------------------------------------------ */
/* 工具:act(注入)                                                       */
/* ------------------------------------------------------------------ */

static uint8_t parse_modifiers(const cJSON *args)
{
    uint8_t mod = 0;
    const cJSON *arr = cJSON_GetObjectItem(args, "modifiers");
    if (!cJSON_IsArray(arr)) {
        return 0;
    }
    const cJSON *m = NULL;
    cJSON_ArrayForEach(m, arr) {
        if (!cJSON_IsString(m)) {
            continue;
        }
        const char *s = m->valuestring;
        if (!strcmp(s, "ctrl")) { mod |= IO_HID_MOD_LCTRL; }
        else if (!strcmp(s, "shift")) { mod |= IO_HID_MOD_LSHIFT; }
        else if (!strcmp(s, "alt")) { mod |= IO_HID_MOD_LALT; }
        else if (!strcmp(s, "win")) { mod |= IO_HID_MOD_LGUI; }
    }
    return mod;
}

/*
 * 解析按键名:先当**名字**(SHIFT/ENTER/ESC/...),再当可打印字符。
 *
 * 为什么需要名字:微软拼音的中英切换只在**单独**按 Shift 时触发,而字符表
 * 只有可打印 ASCII —— 没有名字就发不出"裸 Shift"。
 */
static bool resolve_key(const char *name, uint8_t *mod, uint8_t *key)
{
    if (!name || !name[0]) {
        return false;
    }
    if (io_hid_key_for_named(name, mod, key)) {
        return true;
    }
    return io_hid_key_for_char(name[0], mod, key);
}

/* 执行一个动作。返回 ESP_OK / 错误码。 */
static esp_err_t act_run(const cJSON *args, cJSON *detail)
{
    const cJSON *action = cJSON_GetObjectItem(args, "action");
    if (!cJSON_IsString(action)) {
        return ESP_ERR_INVALID_ARG;
    }
    const char *a = action->valuestring;
    const cJSON *hold = cJSON_GetObjectItem(args, "hold_ms");
    const cJSON *per = cJSON_GetObjectItem(args, "per_char_ms");
    const uint16_t per_char = cJSON_IsNumber(per) ? (uint16_t)per->valuedouble : IO_HID_KEY_HOLD_MS;

    if (!strcmp(a, "type")) {
        const cJSON *text = cJSON_GetObjectItem(args, "text");
        if (!cJSON_IsString(text)) {
            return ESP_ERR_INVALID_ARG;
        }
        cJSON_AddStringToObject(detail, "typed", text->valuestring);
        return io_hid_type_text(text->valuestring, per_char);
    }
    if (!strcmp(a, "tap")) {
        const cJSON *key = cJSON_GetObjectItem(args, "key");
        if (!cJSON_IsString(key) || key->valuestring[0] == '\0') {
            return ESP_ERR_INVALID_ARG;
        }
        uint8_t mod = 0, keycode = 0;
        if (!resolve_key(key->valuestring, &mod, &keycode)) {
            return ESP_ERR_INVALID_ARG;
        }
        mod |= parse_modifiers(args);
        cJSON_AddStringToObject(detail, "key", key->valuestring);
        /* 单键默认按 60 ms(IME 处理得过来);有 modifier 时按组合键时长 */
        const uint16_t hold_ms = cJSON_IsNumber(hold)
                                   ? (uint16_t)hold->valuedouble
                                   : (mod ? IO_HID_CHORD_HOLD_MS : IO_HID_KEY_HOLD_MS);
        /* 有 modifier 时走 hotkey(它会清基线),没有就用 tap */
        return mod ? io_hid_hotkey(mod, keycode, hold_ms)
                   : io_hid_keyboard_tap(keycode, hold_ms);
    }
    if (!strcmp(a, "hotkey")) {
        const cJSON *key = cJSON_GetObjectItem(args, "key");
        if (!cJSON_IsString(key) || key->valuestring[0] == '\0') {
            return ESP_ERR_INVALID_ARG;
        }
        uint8_t mod = 0, keycode = 0;
        if (!resolve_key(key->valuestring, &mod, &keycode)) {
            return ESP_ERR_INVALID_ARG;
        }
        mod |= parse_modifiers(args);
        if (mod == 0) {
            return ESP_ERR_INVALID_ARG;   /* hotkey 必须有 modifier */
        }
        cJSON_AddStringToObject(detail, "key", key->valuestring);
        /* 组合键默认按 600 ms:实测 30 ms 的组合键会被 Windows 整段丢弃 */
        const uint16_t hold_ms = cJSON_IsNumber(hold)
                                   ? (uint16_t)hold->valuedouble
                                   : IO_HID_CHORD_HOLD_MS;
        return io_hid_hotkey(mod, keycode, hold_ms);
    }
    if (!strcmp(a, "move")) {
        const cJSON *dx = cJSON_GetObjectItem(args, "dx");
        const cJSON *dy = cJSON_GetObjectItem(args, "dy");
        if (!cJSON_IsNumber(dx) || !cJSON_IsNumber(dy)) {
            return ESP_ERR_INVALID_ARG;
        }
        cJSON_AddNumberToObject(detail, "dx", dx->valuedouble);
        cJSON_AddNumberToObject(detail, "dy", dy->valuedouble);
        return io_hid_mouse_move((int16_t)dx->valuedouble, (int16_t)dy->valuedouble);
    }
    if (!strcmp(a, "click")) {
        const cJSON *btn = cJSON_GetObjectItem(args, "button");
        const char *b = (cJSON_IsString(btn) && btn->valuestring[0]) ? btn->valuestring : "left";
        uint8_t mask = 0;
        if (!strcmp(b, "left")) { mask = IO_HID_MOUSE_LEFT; }
        else if (!strcmp(b, "right")) { mask = IO_HID_MOUSE_RIGHT; }
        else if (!strcmp(b, "middle")) { mask = IO_HID_MOUSE_MIDDLE; }
        else { return ESP_ERR_INVALID_ARG; }
        cJSON_AddStringToObject(detail, "button", b);
        ESP_RETURN_ON_ERROR(io_hid_mouse_buttons(mask), TAG, "按下失败");
        const uint16_t click_hold = cJSON_IsNumber(hold)
                                       ? (uint16_t)hold->valuedouble
                                       : 30;
        vTaskDelay(pdMS_TO_TICKS(click_hold));
        return io_hid_mouse_buttons(0);
    }
    if (!strcmp(a, "scroll")) {
        const cJSON *dy = cJSON_GetObjectItem(args, "dy");
        if (!cJSON_IsNumber(dy)) {
            return ESP_ERR_INVALID_ARG;
        }
        cJSON_AddNumberToObject(detail, "dy", dy->valuedouble);
        return io_hid_mouse_scroll((int8_t)dy->valuedouble);
    }
    if (!strcmp(a, "macro")) {
        /* 宏一律走 worker task(长动作),不在 httpd task 里执行 ——
           否则会把 httpd 唯一的 task 占住,busy 语义就无从谈起。
           见文件头的"单例执行槽 + worker task"。 */
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_ERR_INVALID_ARG;
}

/*
 * input 工具:往目标机送一个输入动作。单次执行、零重试(失败不自动重发)。
 *
 * 同步模型(2026-10-23):
 *   - **短动作**(tap/hotkey/move/click/scroll/短 type)在 httpd task 里直接执行,
 *     返回执行结果。
 *   - **长动作**(macro 注入类、超过 16 字符的 type)派给 **worker task**,
 *     本次请求**立刻回 accepted**;结果由 agent 回读盘上的标记文件
 *     (INJ.TXT / CAP.TXT)判定 —— 标记文件本来就是状态,不需要 job 状态机。
 *   - 设备是**单例**:同一时刻只跑一个动作。忙时再来一个动作请求 →
 *     **直接回 busy**,不排队执行。
 *   - 读类工具(ls / read / capture / status)在动作执行期间**照常服务**。
 *
 * 注意:动作不再自动回传"注入前后两张截图"。异步模型下"后一张"没有意义
 * (请求早就返回了),由 agent 自己用 capture 取前后帧来比对。
 */
static io_tool_out_t tool_input(const cJSON *args)
{
    cJSON *out = cJSON_CreateObject();

    /* 链路不可用就直接报错,不要盲发 */
    if (!io_usb_link_alive()) {
        cJSON_AddBoolToObject(out, "ok", false);
        cJSON_AddStringToObject(out, "error", "usb link is not alive (host suspended or unplugged)");
        return tool_out(out, true);
    }
    if (!io_hid_ready()) {
        cJSON_AddBoolToObject(out, "ok", false);
        cJSON_AddStringToObject(out, "error", "HID endpoint is busy (host is not polling it)");
        return tool_out(out, true);
    }

    const cJSON *action = cJSON_GetObjectItem(args, "action");
    if (!cJSON_IsString(action)) {
        cJSON_AddBoolToObject(out, "ok", false);
        cJSON_AddStringToObject(out, "error", "missing action");
        return tool_out(out, true);
    }
    const char *a = action->valuestring;
    const cJSON *text = cJSON_GetObjectItem(args, "text");
    const bool is_macro = !strcmp(a, "macro");
    const bool long_type = (!strcmp(a, "type") && cJSON_IsString(text) &&
                            strlen(text->valuestring) > IO_JOB_TYPE_ASYNC_CHARS);

    cJSON *detail = cJSON_CreateObject();
    io_job_t job;
    memset(&job, 0, sizeof(job));

    if (is_macro) {
        /* 宏名先校验,别把不认识的宏塞进队列 */
        const cJSON *m = cJSON_GetObjectItem(args, "macro");
        if (!cJSON_IsString(m) || !m->valuestring[0]) {
            cJSON_Delete(detail);
            cJSON_AddBoolToObject(out, "ok", false);
            cJSON_AddStringToObject(out, "error", "missing macro");
            return tool_out(out, true);
        }
        if (strcmp(m->valuestring, "selftest") &&
            strcmp(m->valuestring, "launch_watcher") &&
            strcmp(m->valuestring, "focus_reset") &&
            strcmp(m->valuestring, "ime_toggle")) {
            cJSON_Delete(detail);
            cJSON_AddBoolToObject(out, "ok", false);
            cJSON_AddStringToObject(out, "error", "unknown macro");
            return tool_out(out, true);
        }
        job.kind = IO_JOB_KIND_MACRO;
        snprintf(job.text, sizeof(job.text), "%s", m->valuestring);
        cJSON_AddStringToObject(detail, "macro", m->valuestring);
        /* "ime":"toggle" = 打字前单按一次 Shift(把"新框 = 当前态(中文)"切到英文)。
         * 默认不按:调用方应该先用 macro=selftest 探针测出当前态(探针不过就说明
         * 是中文态,这时 launch 用 "toggle" —— 新框继承当前态,Shift 一下就切到英文)。
         */
        const cJSON *ime = cJSON_GetObjectItem(args, "ime");
        job.ime_toggle = (cJSON_IsString(ime) && !strcmp(ime->valuestring, "toggle"));
        cJSON_AddBoolToObject(detail, "ime_toggle", job.ime_toggle);
    } else if (long_type) {
        job.kind = IO_JOB_KIND_TEXT;
        snprintf(job.text, sizeof(job.text), "%s", text->valuestring);
        cJSON_AddStringToObject(detail, "typed", text->valuestring);
    }

    /* 参数都校验过了,这时才抢单例槽:忙就直接回 busy,不排队执行 */
    if (!op_slot_try()) {
        cJSON_Delete(detail);
        cJSON_AddBoolToObject(out, "ok", false);
        cJSON_AddBoolToObject(out, "busy", true);
        cJSON_AddStringToObject(out, "error", "device busy (another action is running)");
        return tool_out(out, true);
    }

    if (is_macro || long_type) {
        /* ---- 长动作:派给 worker,立刻回 accepted(不等结果) ---- */
        if (xQueueSend(s_job_q, &job, 0) != pdTRUE) {
            op_slot_release();
            cJSON_Delete(detail);
            cJSON_AddBoolToObject(out, "ok", false);
            cJSON_AddStringToObject(out, "error", "job queue full");
            return tool_out(out, true);
        }
        /* 槽由 worker 在动作跑完后释放 */
        cJSON_AddItemToObject(out, "action", detail);
        cJSON_AddBoolToObject(out, "ok", true);
        cJSON_AddBoolToObject(out, "accepted", true);
        cJSON_AddBoolToObject(out, "busy", true);
        cJSON_AddStringToObject(out, "note",
                               "accepted and running in background; poll status "
                               "(busy=false) then read the marker file (INJ.TXT / "
                               "CAP.TXT) to judge the outcome");
        return tool_out(out, false);
    }

    /* ---- 短动作:同步执行 ---- */
    const esp_err_t rc = act_run(args, detail);
    op_slot_release();

    cJSON_AddItemToObject(out, "action", detail);
    cJSON_AddBoolToObject(out, "ok", (rc == ESP_OK));
    if (rc != ESP_OK) {
        cJSON_AddStringToObject(out, "error", esp_err_to_name(rc));
    }
    return tool_out(out, rc != ESP_OK);
}

/* ------------------------------------------------------------------ */
/* 工具:ls / read / capture / update                                     */
/* ------------------------------------------------------------------ */

/*
 * ls —— 列共享盘。
 *
 * 盘是目标机可见的那张 FAT16 卷:watcher 把帧写在上面,宿主也能往上写文件。
 * agent 要知道"盘上现在有什么、哪个最新"就得问它。
 */
static io_tool_out_t tool_ls(const cJSON *args)
{
    (void)args;
    cJSON *o = cJSON_CreateObject();

    /* 静态缓冲:httpd 是单 task,不会并发;栈上放不下。 */
    static io_file_info_t files[IO_DIR_MAX];
    const size_t total = io_disk_list_files(files, IO_DIR_MAX);

    cJSON *arr = cJSON_CreateArray();
    for (size_t i = 0; i < total && i < IO_DIR_MAX; i++) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "name", files[i].name);
        cJSON_AddNumberToObject(e, "size", (double)files[i].size);
        cJSON_AddNumberToObject(e, "mtime", (double)files[i].mtime);
        cJSON_AddBoolToObject(e, "frame", name_is_frame(files[i].name));
        cJSON_AddItemToArray(arr, e);
    }
    cJSON_AddItemToObject(o, "files", arr);
    cJSON_AddNumberToObject(o, "count", (double)total);
    cJSON_AddBoolToObject(o, "ok", true);
    if (total > IO_DIR_MAX) {
        cJSON_AddStringToObject(o, "note", "listing truncated");
    }
    return tool_out(o, false);
}

/*
 * read —— 读共享盘上的某个文件。
 *
 * 小文件直接内联(文本块);图片走 MCP 的 image 块;太大的只给 URL。
 */
static io_tool_out_t tool_read(const cJSON *args)
{
    cJSON *o = cJSON_CreateObject();

    const cJSON *n = cJSON_GetObjectItem(args, "name");
    if (!cJSON_IsString(n) || !n->valuestring[0]) {
        cJSON_AddBoolToObject(o, "ok", false);
        cJSON_AddStringToObject(o, "error", "missing name");
        return tool_out(o, true);
    }
    const char *name = n->valuestring;

    io_file_info_t info;
    if (!io_disk_find_file(name, &info)) {
        cJSON_AddBoolToObject(o, "ok", false);
        cJSON_AddStringToObject(o, "file", name);
        cJSON_AddStringToObject(o, "error", "no such file on the volume");
        return tool_out(o, true);
    }

    cJSON_AddStringToObject(o, "file", info.name);
    cJSON_AddNumberToObject(o, "size", (double)info.size);
    cJSON_AddNumberToObject(o, "mtime", (double)info.mtime);

    /* 太大就只给 URL(客户端自己去 GET /files/<name>) */
    if (info.size > IO_FILE_MAX_INLINE) {
        char *url = files_url(info.name);
        cJSON_AddBoolToObject(o, "ok", true);
        if (url) {
            cJSON_AddStringToObject(o, "url", url);
            free(url);
        }
        cJSON_AddStringToObject(o, "note", "too large to inline; fetch the url");
        return tool_out(o, false);
    }

    static uint8_t buf[IO_FILE_BUF_MAX];
    const size_t len = read_shared_file(info.name, buf, sizeof(buf));
    if (len == 0) {
        cJSON_AddBoolToObject(o, "ok", false);
        cJSON_AddStringToObject(o, "error", "read failed (file gone or empty?)");
        return tool_out(o, true);
    }
    cJSON_AddNumberToObject(o, "bytes", (double)len);

    if (bytes_are_jpeg(buf, len)) {
        char *b64 = b64_encode(buf, len);
        if (!b64) {
            cJSON_AddBoolToObject(o, "ok", false);
            cJSON_AddStringToObject(o, "error", "out of memory");
            return tool_out(o, true);
        }
        cJSON_AddStringToObject(o, "mime", "image/jpeg");
        cJSON_AddStringToObject(o, "image_b64", b64);
        free(b64);
        io_tool_out_t out = { o, false, true, "image/jpeg" };
        return out;
    }

    /* 文本:补 NUL 收尾,再给出去 */
    buf[len < sizeof(buf) ? len : sizeof(buf) - 1] = '\0';
    cJSON_AddStringToObject(o, "text", (const char *)buf);
    cJSON_AddBoolToObject(o, "ok", true);
    return tool_out(o, false);
}

/*
 * capture —— 取最新一帧。
 *
 * "最新"按**最后写入时间**判:帧是目标机上的 watcher 写的,FAT16 目录项里
 * 带时间戳,所以谁的 mtime 最大谁就是最新的那一帧。
 */
static io_tool_out_t tool_capture(const cJSON *args)
{
    (void)args;
    cJSON *o = cJSON_CreateObject();

    static io_file_info_t files[IO_DIR_MAX];
    const size_t total = io_disk_list_files(files, IO_DIR_MAX);

    int best = -1;
    for (size_t i = 0; i < total && i < IO_DIR_MAX; i++) {
        if (!name_is_frame(files[i].name)) {
            continue;
        }
        if (best < 0 || files[i].mtime > files[best].mtime) {
            best = (int)i;
        }
    }
    if (best < 0) {
        cJSON_AddBoolToObject(o, "ok", false);
        cJSON_AddStringToObject(o, "error",
                               "no frame on the volume (is the watcher running on the target?)");
        return tool_out(o, true);
    }

    const io_file_info_t *f = &files[best];
    cJSON_AddStringToObject(o, "file", f->name);
    cJSON_AddNumberToObject(o, "size", (double)f->size);
    cJSON_AddNumberToObject(o, "mtime", (double)f->mtime);

    if (f->size > IO_FILE_MAX_INLINE) {
        char *url = files_url(f->name);
        cJSON_AddBoolToObject(o, "ok", true);
        if (url) {
            cJSON_AddStringToObject(o, "url", url);
            free(url);
        }
        cJSON_AddStringToObject(o, "note", "frame too large to inline; fetch the url");
        return tool_out(o, false);
    }

    static uint8_t buf[IO_FILE_BUF_MAX];
    const size_t len = read_shared_file(f->name, buf, sizeof(buf));
    if (len == 0) {
        cJSON_AddBoolToObject(o, "ok", false);
        cJSON_AddStringToObject(o, "error", "read failed (frame gone?)");
        return tool_out(o, true);
    }
    cJSON_AddNumberToObject(o, "bytes", (double)len);

    if (bytes_are_jpeg(buf, len)) {
        char *b64 = b64_encode(buf, len);
        if (!b64) {
            cJSON_AddBoolToObject(o, "ok", false);
            cJSON_AddStringToObject(o, "error", "out of memory");
            return tool_out(o, true);
        }
        cJSON_AddStringToObject(o, "mime", "image/jpeg");
        cJSON_AddStringToObject(o, "image_b64", b64);
        free(b64);
        io_tool_out_t out = { o, false, true, "image/jpeg" };
        return out;
    }

    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddStringToObject(o, "note", "latest frame is not a JPEG; read it as bytes");
    return tool_out(o, false);
}

/*
 * update —— 更新一个 flash 槽位。
 *
 * 写的是 **flash**(A/B 快照),**下次开机**才生效:RAM disk 里的那份是开机
 * 铺开的副本,运行期改它等于和目标机抢同一批簇(见"真源/副本契约")。
 */
static io_tool_out_t tool_update(const cJSON *args)
{
    cJSON *o = cJSON_CreateObject();

    const cJSON *slot = cJSON_GetObjectItem(args, "slot");
    const uint32_t slot_index = cJSON_IsNumber(slot) ? (uint32_t)slot->valuedouble : 0;

    const cJSON *text = cJSON_GetObjectItem(args, "text");
    const cJSON *b64 = cJSON_GetObjectItem(args, "base64");
    if (!cJSON_IsString(text) && !cJSON_IsString(b64)) {
        cJSON_AddBoolToObject(o, "ok", false);
        cJSON_AddStringToObject(o, "error", "need either 'text' or 'base64'");
        return tool_out(o, true);
    }

    const io_slot_desc_t *sd = io_disk_slot_at(slot_index);
    if (!sd || sd->kind != IO_SLOT_KIND_SLOT) {
        cJSON_AddBoolToObject(o, "ok", false);
        cJSON_AddStringToObject(o, "error", "no such slot");
        return tool_out(o, true);
    }

    uint8_t *buf = (uint8_t *)heap_caps_malloc(sd->capacity, MALLOC_CAP_SPIRAM);
    if (!buf) {
        cJSON_AddBoolToObject(o, "ok", false);
        cJSON_AddStringToObject(o, "error", "out of memory");
        return tool_out(o, true);
    }
    size_t len = 0;
    if (cJSON_IsString(text)) {
        len = strlen(text->valuestring);
        if (len > sd->capacity) { len = sd->capacity; }
        memcpy(buf, text->valuestring, len);
    } else {
        const int n = b64_decode(b64->valuestring, buf, sd->capacity);
        if (n < 0) {
            heap_caps_free(buf);
            cJSON_AddBoolToObject(o, "ok", false);
            cJSON_AddStringToObject(o, "error", "bad base64");
            return tool_out(o, true);
        }
        len = (size_t)n;
    }

    /* 写 flash 也是**原子操作** → 走单例槽:忙就直接回 busy,不排队执行。
     * 放在参数都校验完之后、真正写之前,免得中途早退把槽漏掉。 */
    if (!op_slot_try()) {
        heap_caps_free(buf);
        cJSON_AddBoolToObject(o, "ok", false);
        cJSON_AddBoolToObject(o, "busy", true);
        cJSON_AddStringToObject(o, "error", "device busy (another action is running)");
        return tool_out(o, true);
    }
    const esp_err_t rc = io_payload_store(slot_index, buf, len);
    op_slot_release();
    heap_caps_free(buf);

    cJSON_AddBoolToObject(o, "ok", (rc == ESP_OK));
    cJSON_AddStringToObject(o, "slot", sd->name);
    cJSON_AddNumberToObject(o, "len", (double)len);
    if (rc != ESP_OK) {
        cJSON_AddStringToObject(o, "error", esp_err_to_name(rc));
        return tool_out(o, true);
    }
    cJSON_AddNumberToObject(o, "payload_seq", (double)io_payload_active_seq());
    cJSON_AddStringToObject(o, "note",
                           "stored in flash only -- the RAM disk copy is written at next boot");
    return tool_out(o, false);
}

/*
 * flash —— 给设备刷**整机固件**(OTA)。
 *
 * 和 update 的区别(两个都是"写 flash",但写的东西不同):
 *   update : 写**一个槽位里的引导文件**(WATCH.PS1 / AGENT.SH / AGENT.EXE),
 *            内容是给目标机跑的脚本,下次开机的 overlay 铺到 RAM disk。
 *   flash  : 换掉**整个 app 镜像**,刷完设备重启就跑新固件。
 *
 * 为什么必须有它:单 Type-C 口的 dongle 只有一个口,插在目标机上做
 * HID+MSC ⇒ 开发机**烧不进去**了。固件更新必须走网络。
 *
 * 设备是**去开发机上拉**镜像(pull),不是开发机往里推(push):push 会把
 * 设备唯一的 httpd task 占住 900 KB 的传输时长(那个 httpd 是单 task,
 * 占住 = MCP 整个服务卡死)。
 */
static io_tool_out_t tool_flash(const cJSON *args)
{
    cJSON *o = cJSON_CreateObject();

    const cJSON *url = cJSON_GetObjectItem(args, "url");
    if (!cJSON_IsString(url) || !url->valuestring[0]) {
        cJSON_AddBoolToObject(o, "ok", false);
        cJSON_AddStringToObject(o, "error", "missing url");
        return tool_out(o, true);
    }

    if (io_ota_busy()) {
        cJSON_AddBoolToObject(o, "ok", false);
        cJSON_AddBoolToObject(o, "busy", true);
        cJSON_AddStringToObject(o, "error", "an OTA is already running");
        return tool_out(o, true);
    }

    const esp_err_t rc = io_ota_start(url->valuestring);
    if (rc != ESP_OK) {
        cJSON_AddBoolToObject(o, "ok", false);
        cJSON_AddStringToObject(o, "error", esp_err_to_name(rc));
        return tool_out(o, true);
    }

    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddBoolToObject(o, "accepted", true);
    cJSON_AddStringToObject(o, "url", url->valuestring);
    cJSON_AddStringToObject(o, "note",
                           "accepted; downloading in the background. The image is buffered "
                           "in PSRAM first, so the device stays responsive while it "
                           "downloads; it goes briefly unresponsive for a few seconds "
                           "while the image is written to flash. When the download "
                           "finishes the device REBOOTS and this connection drops. "
                           "Poll status for ota.state / ota.received, then after the "
                           "reboot check that ota.boot flipped to the other slot -- that "
                           "is the proof the new image is the one running.");
    return tool_out(o, false);
}

/* ------------------------------------------------------------------ */
/* tools/list                                                           */
/* ------------------------------------------------------------------ */

static cJSON *tool_schema_obj(const char *type)
{
    cJSON *s = cJSON_CreateObject();
    cJSON_AddStringToObject(s, "type", type);
    return s;
}

static cJSON *tool_schema_str(const char *desc)
{
    cJSON *s = cJSON_CreateObject();
    cJSON_AddStringToObject(s, "type", "string");
    cJSON_AddStringToObject(s, "description", desc);
    return s;
}

static cJSON *tool_schema_num(const char *desc)
{
    cJSON *s = cJSON_CreateObject();
    cJSON_AddStringToObject(s, "type", "number");
    cJSON_AddStringToObject(s, "description", desc);
    return s;
}

/*
 * 往数组里加一个工具。
 *
 * ⚠ **props 的所有权转移给本函数**(它被挂进 schema)—— 调用方**不要再
 * cJSON_Delete(props)**。踩过这个坑:调用方删了一次、schema 又删一次 =>
 * 双重释放 => 堆损坏 => 设备在 tools/list 时崩溃重启。
 */
static cJSON *add_tool(cJSON *arr, const char *name, const char *title, const char *desc,
                       cJSON *props, const char *required)
{
    cJSON *t = cJSON_CreateObject();
    cJSON_AddStringToObject(t, "name", name);
    if (title) {
        /* title 是给人看的名字(规范里 name 是标识、title 是显示名) */
        cJSON_AddStringToObject(t, "title", title);
    }
    cJSON_AddStringToObject(t, "description", desc);
    cJSON *schema = cJSON_CreateObject();
    cJSON_AddStringToObject(schema, "type", "object");
    if (props) {
        cJSON_AddItemToObject(schema, "properties", props);
    }
    if (required) {
        cJSON *req = cJSON_CreateArray();
        cJSON_AddItemToArray(req, cJSON_CreateString(required));
        cJSON_AddItemToObject(schema, "required", req);
    }
    cJSON_AddItemToObject(t, "inputSchema", schema);
    cJSON_AddItemToArray(arr, t);
    return t;
}

/*
 * 给工具挂 outputSchema。
 *
 * 规范:提供了 outputSchema 的话,服务端**必须**返回符合该 schema 的
 * structuredContent,客户端**应该**照它校验。我们每个工具的结果都带 ok,
 * 所以统一声明 object + 必含 ok。
 */
static void add_output_schema(cJSON *tool)
{
    cJSON *schema = cJSON_CreateObject();
    cJSON_AddStringToObject(schema, "type", "object");
    cJSON *props = cJSON_CreateObject();
    cJSON *ok = cJSON_CreateObject();
    cJSON_AddStringToObject(ok, "type", "boolean");
    cJSON_AddItemToObject(props, "ok", ok);
    cJSON_AddItemToObject(schema, "properties", props);
    cJSON *req = cJSON_CreateArray();
    cJSON_AddItemToArray(req, cJSON_CreateString("ok"));
    cJSON_AddItemToObject(schema, "required", req);
    cJSON_AddItemToObject(tool, "outputSchema", schema);
}

/*
 * 工具清单。
 *
 * 命名规则:**全部是动词,全小写单词,零分隔符**。
 * 之前混着斜杠(device/info)、单词(act)、下划线(deploy_payload)三套写法,
 * 同一个 agent 看到要自己猜风格 —— 现在统一成"一个动词 = 一件事"。
 */
static cJSON *build_tools_list(void)
{
    cJSON *tools = cJSON_CreateArray();

    {
        cJSON *p = cJSON_CreateObject();
        cJSON *t = add_tool(tools, "status", "Device status",
                            "Identity, USB/HID link state, LED status code, the flash slot "
                            "table, and whether the watcher is running on the target machine.",
                            p, NULL);
        add_output_schema(t);
    }
    {
        cJSON *p = cJSON_CreateObject();
        cJSON *t = add_tool(tools, "ls", "List the shared disk",
                            "List the files on the dongle's shared disk (the same volume the "
                            "target machine sees): name, size, last-write time, and whether it "
                            "is a frame file.",
                            p, NULL);
        add_output_schema(t);
    }
    {
        cJSON *p = cJSON_CreateObject();
        cJSON_AddItemToObject(p, "name", tool_schema_str(
            "8.3 file name on the shared disk, e.g. CAP.TXT, INJ.TXT, DIFF.IDX, SNAP_P.JPG"));
        cJSON *t = add_tool(tools, "read", "Read a file from the shared disk",
                            "Read one file from the shared disk. Text comes back inline, images "
                            "come back as an MCP image block, and anything too large is "
                            "returned as a URL to GET /files/<name>.",
                            p, "name");
        add_output_schema(t);
    }
    {
        cJSON *p = cJSON_CreateObject();
        cJSON *t = add_tool(tools, "capture", "Capture the latest frame",
                            "Return the newest frame on the shared disk (picked by last-write "
                            "time) as an MCP image block, with its file name, size and "
                            "timestamp. This is the 'see that machine's screen' call.",
                            p, NULL);
        add_output_schema(t);
    }
    {
        cJSON *p = cJSON_CreateObject();
        cJSON_AddItemToObject(p, "action", tool_schema_str(
            "type | tap | hotkey | move | click | scroll | macro"));
        cJSON_AddItemToObject(p, "text", tool_schema_str("text to type (action=type)"));
        cJSON_AddItemToObject(p, "key", tool_schema_str("single character key (action=tap/hotkey)"));
        cJSON_AddItemToObject(p, "modifiers", tool_schema_obj("array of ctrl/shift/alt/win"));
        cJSON_AddItemToObject(p, "dx", tool_schema_num("mouse dx (action=move)"));
        cJSON_AddItemToObject(p, "dy", tool_schema_num("mouse dy (action=move/scroll)"));
        cJSON_AddItemToObject(p, "button", tool_schema_str("left|right|middle (action=click)"));
        cJSON_AddItemToObject(p, "macro", tool_schema_str("focus_reset | selftest | launch_watcher | ime_toggle"));
        cJSON_AddItemToObject(p, "ime", tool_schema_str("'toggle' with macro=launch_watcher: press Shift once before typing (for hosts whose IME is in Chinese mode; default = do not press it)"));
        cJSON_AddItemToObject(p, "hold_ms", tool_schema_num("key hold time, default 60"));
        cJSON_AddItemToObject(p, "per_char_ms", tool_schema_num(
            "per-character hold time in action=type, default 60 (IME needs >= 60 ms "
            "or the input comes out garbled)"));
        cJSON *t = add_tool(tools, "input", "Send one input action to the target machine",
                            "Inject ONE action on the target machine. Single execution, never "
                            "retried. Long actions (macro / long text) are accepted and run in "
                            "background: the reply is {\"accepted\":true,\"busy\":true} and the "
                            "outcome is judged by the marker file (INJ.TXT / CAP.TXT) -- poll "
                            "status until busy=false, then read the marker with 'read'. Short "
                            "actions run inline. If another action is already running the reply "
                            "is {\"busy\":true} (the device is a singleton; it never queues "
                            "actions).",
                            p, "action");
        add_output_schema(t);
    }
    {
        cJSON *p = cJSON_CreateObject();
        cJSON_AddItemToObject(p, "slot", tool_schema_num("slot index, 0=WATCH.PS1 1=AGENT.SH 2=AGENT.EXE"));
        cJSON_AddItemToObject(p, "text", tool_schema_str("plain text payload"));
        cJSON_AddItemToObject(p, "base64", tool_schema_str("base64 payload (alternative to text)"));
        cJSON *t = add_tool(tools, "update", "Update a flash slot",
                            "Store a payload into the flash A/B bank. It takes effect at the "
                            "next boot (the RAM disk copy is written by the boot-time overlay).",
                            p, "slot");
        add_output_schema(t);
    }
    {
        cJSON *p = cJSON_CreateObject();
        cJSON_AddItemToObject(p, "url", tool_schema_str(
            "http(s) URL of the firmware image, e.g. http://192.168.1.5:8000/sophon.bin"));
        cJSON *t = add_tool(tools, "flash", "Flash the whole firmware over WiFi (OTA)",
                            "Replace the device's own app image: the device DOWNLOADS the "
                            "image from the URL, writes it into the other OTA slot, verifies "
                            "it, switches the boot partition and reboots. This is how a "
                            "single-Type-C dongle gets new firmware, since the only USB port "
                            "is occupied by the HID+MSC link to the target machine. The "
                            "reply is {\"accepted\":true}; when the download finishes the "
                            "device REBOOTS and this connection drops -- re-discover it and "
                            "check that the firmware build time changed. A bad image is "
                            "rejected and the current firmware keeps running.",
                            p, "url");
        add_output_schema(t);
    }
    return tools;
}

/* ------------------------------------------------------------------ */
/* 请求加固 + 协议版本协商                                               */
/* ------------------------------------------------------------------ */

/* 这个协议版本我们支持吗? */
static bool proto_supported(const char *v)
{
    if (!v || !*v) {
        return false;
    }
    const size_t n = sizeof(IO_MCP_PROTO_SUPPORTED) / sizeof(IO_MCP_PROTO_SUPPORTED[0]);
    for (size_t i = 0; i < n; i++) {
        if (!strcmp(v, IO_MCP_PROTO_SUPPORTED[i])) {
            return true;
        }
    }
    return false;
}

/*
 * 协议版本协商。
 *
 * MCP 规范:客户端 initialize 时带 protocolVersion;服务端支持就**原样回**,
 * 不支持就回自己支持的版本,由客户端决定要不要继续(客户端可以断开重连)。
 * 之前是固定回一个版本号,客户端要什么版本都假装同意。
 */
static const char *proto_negotiate(const char *requested)
{
    return proto_supported(requested) ? requested : IO_MCP_PROTO;
}

/*
 * Host / Origin 绑定(DNS rebinding 防护)。
 *
 * 威胁模型:受害者浏览器打开一个恶意页面,页面里的 JS 去 POST
 * http://<设备IP>:8080/mcp —— 浏览器会带 `Host: <设备IP>:8080`
 * (所以单看 Host 没用,浏览器会把 Host 设成目标),但会带
 * `Origin: http://evil.example`。我们的调用方是 native agent,
 * **永远不发 Origin**,所以规则很干脆:
 *   - Host 的主机部分必须是"我们自己"(当前 IP / 设备名 / 设备名.local)
 *   - 带了 Origin 就必须也是我们自己,否则拒
 */
static bool host_is_ours(const char *host)
{
    if (!host || !*host) {
        return false;
    }
    /* 取主机部分:去掉端口和 DNS 根点 */
    char h[160];
    size_t n = strlen(host);
    if (n >= sizeof(h)) {
        n = sizeof(h) - 1;
    }
    memcpy(h, host, n);
    h[n] = '\0';
    char *colon = strrchr(h, ':');
    if (colon) {
        *colon = '\0';
    }
    n = strlen(h);
    if (n && h[n - 1] == '.') {
        h[n - 1] = '\0';
    }

    const char *ip = io_net_ip();
    if (ip && *ip && !strcmp(h, ip)) {
        return true;
    }

    const char *name = io_cfg_device_name();
    if (name && *name) {
        if (!strcasecmp(h, name)) {
            return true;
        }
        /* "<name>.local" —— mDNS 名。
         * ★ ".local" 是 6 个字符 + 结尾 NUL = 7 字节,拷 6 字节会漏掉 NUL,
         *   后面接栈垃圾,strcasecmp 必然不等(踩过)。 */
        char fqdn[192];
        const size_t nl = strlen(name);
        if (nl + sizeof(".local") <= sizeof(fqdn)) {
            memcpy(fqdn, name, nl);
            memcpy(fqdn + nl, ".local", sizeof(".local"));   /* 含结尾 NUL */
            if (!strcasecmp(h, fqdn)) {
                return true;
            }
        }
    }
    return false;
}

/* Origin 形如 "http://host:port" —— 只取 host 部分判断 */
static bool origin_is_ours(const char *origin)
{
    if (!origin || !*origin) {
        return true;             /* 没有 Origin = native agent,放行 */
    }
    const char *sep = strstr(origin, "://");
    const char *hostpart = sep ? sep + 3 : origin;
    char h[160];
    size_t n = strlen(hostpart);
    if (n >= sizeof(h)) {
        n = sizeof(h) - 1;
    }
    memcpy(h, hostpart, n);
    h[n] = '\0';
    char *slash = strchr(h, '/');
    if (slash) {
        *slash = '\0';
    }
    return host_is_ours(h);
}

/* Content-Type 必须是 application/json(允许带 charset 参数) */
static bool content_type_is_json(const char *ct)
{
    if (!ct) {
        return false;
    }
    const char *p = ct;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (strncasecmp(p, "application/json", 16) != 0) {
        return false;
    }
    p += 16;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    return *p == '\0' || *p == ';' || *p == ',';
}

/*
 * 取请求头的值(拷贝一份)。
 *
 * IDF 的 httpd_req_get_hdr_value_str(r, field, val, val_size) 是**写进调用方的
 * buffer**,没有"返回指针"的版本;而且文档明说:**httpd_resp_send() 之后请求头
 * 会被清空** —— 所以必须在应答之前取出来。
 */
static bool req_header(httpd_req_t *req, const char *field, char *buf, size_t buf_size)
{
    if (!buf || buf_size == 0) {
        return false;
    }
    buf[0] = '\0';
    const esp_err_t rc = httpd_req_get_hdr_value_str(req, field, buf, buf_size);
    if (rc != ESP_OK) {
        buf[0] = '\0';
        return false;
    }
    return true;
}

/*
 * 抽干 body(有上限)。
 *
 * 为什么拒绝之前要先抽干:服务端带着**未读的请求体**关连接时,TCP 会发 RST,
 * 客户端还没读到的响应就被丢弃 —— 客户端拿到的是"连接被重置"而不是状态码。
 * 先把 body 读掉再回错误码,客户端才能干净地读到 413/403/415。
 * 抽干量有上限(IO_MCP_MAX_DRAIN),超大 body 仍然快速拒绝。
 */
static void http_drain_body(httpd_req_t *req)
{
    size_t left = req->content_len;
    if (left > IO_MCP_MAX_DRAIN) {
        left = IO_MCP_MAX_DRAIN;
    }
    char sink[512];
    int timeouts = 0;
    while (left > 0) {
        const size_t want = (left < sizeof(sink)) ? left : sizeof(sink);
        const int r = httpd_req_recv(req, sink, want);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 2) {
                continue;
            }
            break;
        }
        timeouts = 0;
        left -= (size_t)r;
    }
}

/* Host / Origin 检查(GET 端点也要 —— /files 装的是桌面帧) */
static bool http_guard_host_origin(httpd_req_t *req)
{
    /*
     * 缓冲放 static 而不是栈上:httpd 是**单 task**,同一时刻只有一个请求在跑,
     * 静态缓冲不会有并发问题。而 httpd 任务的栈只有几 KB(见下面的 stack_size),
     * 每个请求路径都省一点栈是有意义的。
     */
    static char host[160];
    static char origin[192];

    /* 1) Host 必须是我们自己 */
    const bool have_host = req_header(req, "Host", host, sizeof(host));
    if (!have_host || !host_is_ours(host)) {
        ESP_LOGW(TAG, "reject: Host not allowed (%s)", have_host ? host : "(none)");
        http_drain_body(req);
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "host not allowed");
        return false;
    }

    /* 2) Origin(如果有)必须也是我们自己 */
    if (req_header(req, "Origin", origin, sizeof(origin)) && origin[0] &&
        !origin_is_ours(origin)) {
        ESP_LOGW(TAG, "reject: cross-origin request (%s)", origin);
        http_drain_body(req);
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "origin not allowed");
        return false;
    }
    return true;
}

/* POST 专用的完整加固:Host/Origin + Content-Type + body 大小 */
static bool http_guard_request(httpd_req_t *req)
{
    if (!http_guard_host_origin(req)) {
        return false;
    }

    /* 3) Content-Type 必须是 application/json */
    static char ct[64];
    if (!req_header(req, "Content-Type", ct, sizeof(ct)) || !content_type_is_json(ct)) {
        ESP_LOGW(TAG, "reject: bad Content-Type (%s)", ct[0] ? ct : "(none)");
        http_drain_body(req);
        httpd_resp_set_status(req, "415 Unsupported Media Type");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "Content-Type must be application/json\n");
        return false;
    }

    /* 4) body 大小上限 */
    if (req->content_len > IO_MCP_MAX_BODY) {
        ESP_LOGW(TAG, "reject: body too large (%d > %d)",
                 (int)req->content_len, (int)IO_MCP_MAX_BODY);
        http_drain_body(req);
        httpd_resp_send_err(req, HTTPD_413_CONTENT_TOO_LARGE, "body too large");
        return false;
    }

    return true;
}

/* ------------------------------------------------------------------ */
/* JSON-RPC 分发                                                        */
/* ------------------------------------------------------------------ */

/*
 * 分发一个 JSON-RPC 请求,返回**完整的应答信封**(成功是 result,失败是 error)。
 *
 * ★ 为什么要"完整信封"而不是"结果体"★
 *   JSON-RPC 2.0 里 error 和 result 是**同级**的两种应答:
 *     {"jsonrpc":"2.0","id":1,"result":{...}}
 *     {"jsonrpc":"2.0","id":1,"error":{"code":-32602,"message":"..."}}
 *   之前这个函数对普通调用返回"结果体",对未知工具返回"完整错误信封",
 *   调用方不知道该用哪种,一律包了一层 rpc_ok() ⇒ 错误被塞进了 result 里:
 *     {"result":{"jsonrpc":"2.0","id":1,"error":{...}}}
 *   客户端按规范只认顶层的 error ⇒ **未知工具看起来像"调用成功了"**。
 *   现在统一:这个函数只产出完整信封,调用方原样发出。
 *
 * @return 完整应答信封(调用方负责 cJSON_Delete);返回 NULL 表示"无应答"
 */
static cJSON *dispatch_rpc(const cJSON *req)
{
    const cJSON *method = cJSON_GetObjectItem(req, "method");
    const cJSON *id = cJSON_GetObjectItem(req, "id");
    if (!cJSON_IsString(method)) {
        return rpc_error(id, -32600, "Invalid Request: missing method");
    }
    const char *m = method->valuestring;

    cJSON *result = NULL;

    if (!strcmp(m, "initialize")) {
        /*
         * 协议版本协商:客户端报自己要的版本,我们支持就**原样回**,
         * 不支持就回最新的那个(由客户端决定要不要继续)。
         * 之前是固定回一个版本号,等于假装什么都同意。
         */
        const cJSON *ip = cJSON_GetObjectItem(req, "params");
        const cJSON *want = cJSON_GetObjectItem(ip, "protocolVersion");
        const char *chosen = proto_negotiate(cJSON_IsString(want) ? want->valuestring : NULL);
        if (cJSON_IsString(want) && !proto_supported(want->valuestring)) {
            ESP_LOGW(TAG, "initialize: client asked %s, we answer %s",
                     want->valuestring, chosen);
        }
        result = cJSON_CreateObject();
        cJSON_AddStringToObject(result, "protocolVersion", chosen);
        cJSON *caps = cJSON_CreateObject();
        cJSON *tools = cJSON_CreateObject();
        cJSON_AddBoolToObject(tools, "listChanged", false);
        cJSON_AddItemToObject(caps, "tools", tools);
        cJSON_AddItemToObject(result, "capabilities", caps);
        cJSON *si = cJSON_CreateObject();
        cJSON_AddStringToObject(si, "name", IO_MCP_SERVER_NAME);
        cJSON_AddStringToObject(si, "version", IO_MCP_SERVER_VER);
        cJSON_AddItemToObject(result, "serverInfo", si);
    } else if (!strcmp(m, "ping")) {
        result = cJSON_CreateObject();
    } else if (!strcmp(m, "tools/list")) {
        result = cJSON_CreateObject();
        cJSON_AddItemToObject(result, "tools", build_tools_list());
    } else if (!strcmp(m, "tools/call")) {
        const cJSON *params = cJSON_GetObjectItem(req, "params");
        const cJSON *tname = cJSON_GetObjectItem(params, "name");
        const cJSON *targs = cJSON_GetObjectItem(params, "arguments");
        if (!cJSON_IsString(tname)) {
            return rpc_error(id, -32602, "Invalid params: missing tool name");
        }
        if (!cJSON_IsObject(targs)) {
            targs = NULL;
        }
        const char *n = tname->valuestring;

        io_tool_out_t out;
        if (!strcmp(n, "status")) {
            out = tool_status();
        } else if (!strcmp(n, "ls")) {
            out = tool_ls(targs);
        } else if (!strcmp(n, "read")) {
            out = tool_read(targs);
        } else if (!strcmp(n, "capture")) {
            out = tool_capture(targs);
        } else if (!strcmp(n, "input")) {
            out = tool_input(targs);
        } else if (!strcmp(n, "update")) {
            out = tool_update(targs);
        } else if (!strcmp(n, "flash")) {
            out = tool_flash(targs);
        } else {
            /*
             * 未知工具是**协议错误**(规范:未知工具用 JSON-RPC error,
             * 工具执行错误才用 isError)⇒ 回 -32602 Invalid params。
             * 注意:这里返回的是**完整错误信封**,不能被包进 result。
             */
            char msg[128];
            snprintf(msg, sizeof(msg), "Unknown tool: %s", n);
            return rpc_error(id, -32602, msg);
        }
        result = tool_result(&out);
        cJSON_Delete(out.payload);
    } else {
        return rpc_error(id, -32601, "method not found");
    }

    return rpc_ok(id, result);
}

static cJSON *rpc_error(const cJSON *id, int code, const char *msg)
{
    cJSON *out = cJSON_CreateObject();
    cJSON_AddStringToObject(out, "jsonrpc", "2.0");
    if (id) {
        cJSON_AddItemToObject(out, "id", cJSON_Duplicate(id, true));
    } else {
        cJSON_AddNullToObject(out, "id");
    }
    cJSON *e = cJSON_CreateObject();
    cJSON_AddNumberToObject(e, "code", code);
    cJSON_AddStringToObject(e, "message", msg);
    cJSON_AddItemToObject(out, "error", e);
    return out;
}

static cJSON *rpc_ok(const cJSON *id, cJSON *result)
{
    cJSON *out = cJSON_CreateObject();
    cJSON_AddStringToObject(out, "jsonrpc", "2.0");
    if (id) {
        cJSON_AddItemToObject(out, "id", cJSON_Duplicate(id, true));
    } else {
        cJSON_AddNullToObject(out, "id");
    }
    cJSON_AddItemToObject(out, "result", result ? result : cJSON_CreateObject());
    return out;
}

/* ------------------------------------------------------------------ */
/* HTTP handlers                                                        */
/* ------------------------------------------------------------------ */

static esp_err_t http_mcp_post(httpd_req_t *req)
{
    /* 请求加固:Host / Origin / Content-Type / body 大小,任一不合就拒 */
    if (!http_guard_request(req)) {
        return ESP_FAIL;
    }

    char *body = NULL;
    if (req->content_len > 0) {
        body = (char *)malloc(req->content_len + 1);
        if (!body) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
            return ESP_FAIL;
        }
        int got = 0;
        int timeouts = 0;
        while (got < (int)req->content_len) {
            const int r = httpd_req_recv(req, body + got, req->content_len - got);
            if (r <= 0) {
                /*
                 * 超时要**有上限**:httpd 只有一个 task,这里死循环 = 整个
                 * HTTP 服务永久卡死(症状:设备还活着,但新连接被拒/挂死)。
                 * 客户端发一半 body 就静默时就会踩到。
                 */
                if (r == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 2) {
                    continue;
                }
                free(body);
                httpd_resp_send_err(req,
                                    timeouts ? HTTPD_408_REQ_TIMEOUT
                                             : HTTPD_400_BAD_REQUEST,
                                    "recv failed");
                return ESP_FAIL;
            }
            timeouts = 0;
            got += r;
        }
        body[got] = '\0';
    }

    cJSON *req_json = body ? cJSON_Parse(body) : NULL;
    free(body);
    if (!req_json) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
        return ESP_FAIL;
    }

    /*
     * MCP-Protocol-Version 头:客户端在 initialize **之后**的每个请求都要带,
     * 值就是协商出来的那个版本。我们不认识的版本直接拒(规范建议如此)。
     * initialize 自己不查这个头 —— 它的版本在 body 里,走上面的协商。
     */
    {
        const cJSON *m0 = cJSON_GetObjectItem(req_json, "method");
        const bool is_init = cJSON_IsString(m0) && !strcmp(m0->valuestring, "initialize");
        if (!is_init) {
            char pv[32];
            if (req_header(req, "MCP-Protocol-Version", pv, sizeof(pv)) && pv[0] &&
                !proto_supported(pv)) {
                ESP_LOGW(TAG, "reject: unsupported MCP-Protocol-Version (%s)", pv);
                cJSON_Delete(req_json);
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                    "unsupported protocol version");
                return ESP_FAIL;
            }
        }
    }

    char *resp = NULL;
    if (cJSON_IsArray(req_json)) {
        /* 批量请求 */
        cJSON *arr = cJSON_CreateArray();
        const cJSON *one = NULL;
        cJSON_ArrayForEach(one, req_json) {
            const cJSON *method = cJSON_GetObjectItem(one, "method");
            if (cJSON_IsString(method) && !strcmp(method->valuestring, "notifications/initialized")) {
                continue;   /* 通知,无响应 */
            }
            cJSON *res = dispatch_rpc(one);
            if (res) {
                /* dispatch_rpc 已经给出完整信封(result 或 error),原样放进数组 */
                cJSON_AddItemToArray(arr, res);
            }
        }
        /* 全是通知 ⇒ 按规范回 202 Accepted + 空 body(不是空数组) */
        if (cJSON_GetArraySize(arr) == 0) {
            cJSON_Delete(arr);
            cJSON_Delete(req_json);
            httpd_resp_set_status(req, "202 Accepted");
            httpd_resp_send(req, "", 0);
            return ESP_OK;
        }
        resp = cJSON_PrintUnformatted(arr);
        cJSON_Delete(arr);
    } else {
        const cJSON *method = cJSON_GetObjectItem(req_json, "method");
        cJSON *res = NULL;
        if (cJSON_IsString(method) && !strcmp(method->valuestring, "notifications/initialized")) {
            /*
             * 通知:按规范回 202 Accepted + 空 body。
             * ★ 不再回 `Mcp-Session-Id` —— 我们是**无状态**服务,
             *   之前硬编码一个假 session id 会让客户端以为有会话要维持。
             */
            httpd_resp_set_status(req, "202 Accepted");
            httpd_resp_send(req, "", 0);
            cJSON_Delete(req_json);
            return ESP_OK;
        }
        res = dispatch_rpc(req_json);
        if (res) {
            /* 完整信封:成功的里面是 result,失败的里面是 error(顶层) */
            resp = cJSON_PrintUnformatted(res);
            cJSON_Delete(res);
        }
    }
    cJSON_Delete(req_json);

    httpd_resp_set_type(req, "application/json");
    /* 无状态服务:不回 Mcp-Session-Id(以前硬编码 "ioagent-1",是假的) */
    if (resp) {
        httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
        free(resp);
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no response");
    }
    return ESP_OK;
}

/*
 * GET /mcp -- 规范要求:MCP endpoint 要么支持 GET 开 SSE 流,要么回 405。
 * 我们没有服务器主动推送(SSE)的能力,所以明确回 405,让客户端知道
 * "这个 server 不支持 server->client 的流",而不是收到一个莫名其妙的 404。
 */
static esp_err_t http_mcp_get(httpd_req_t *req)
{
    if (!http_guard_host_origin(req)) {
        return ESP_FAIL;
    }
    httpd_resp_set_status(req, "405 Method Not Allowed");
    httpd_resp_set_hdr(req, "Allow", "POST");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "this server does not offer an SSE stream; use POST\n");
    return ESP_OK;
}

/*
 * GET /files/<name> -- 共享盘上某个文件的**原始字节**。
 *
 * 这是 MCP 之外的旁路:大文件(全分辨率帧约 300 KB)没法 base64 内联进
 * 工具结果,就指到这里;不想配 MCP 客户端的工具也能直接取。
 * 以前这条路叫 /blob/b0..b3,那是把"内部缓存槽位"暴露成了对外契约 ——
 * 现在按**文件名**取,语义直白,而且不用先 read 再取。
 */
static esp_err_t http_files_get(httpd_req_t *req)
{
    /* 共享盘上装的是目标机桌面帧,一样要防 DNS rebinding */
    if (!http_guard_host_origin(req)) {
        return ESP_FAIL;
    }

    const char *tail = req->uri + strlen("/files/");
    if (!*tail) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing file name");
        return ESP_FAIL;
    }

    io_file_info_t info;
    if (!io_disk_find_file(tail, &info)) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no such file on the volume");
        return ESP_FAIL;
    }

    static uint8_t buf[IO_FILE_BUF_MAX];

    const char *ctype = "application/octet-stream";
    /* 头部 sniff:只读开头一点,判断是不是 JPEG */
    size_t head = 0;
    if (io_disk_read_file_at(info.name, 0, buf, sizeof(buf), &head) != ESP_OK || head == 0) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "read failed");
        return ESP_FAIL;
    }
    if (bytes_are_jpeg(buf, head)) {
        ctype = "image/jpeg";
    } else if (!strcmp(info.name, "DIFF.IDX")) {
        ctype = "application/json";
    }
    httpd_resp_set_type(req, ctype);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    /*
     * **流式 + 分块发送**。
     * 一次 send 几百 KB 塞不进 lwIP 的发送缓冲 ⇒ 等 SO_SNDTIMEO 等满 5 秒
     * ⇒ `httpd_sock_err: error in send : 11` ⇒ 连接被掐断,客户端只看到
     * "连接掉了"(真机在并发压测下抓到)。而且设备端只有 512 KB 内部 RAM,
     * 根本放不下整个文件。
     * ⇒ 读一块(16 KB)、发一块(chunked),内存恒定,大文件也能传完。
     */
    size_t off = 0;
    while (off < info.size) {
        size_t got = 0;
        if (io_disk_read_file_at(info.name, (uint32_t)off, buf, sizeof(buf), &got) != ESP_OK ||
            got == 0) {
            break;
        }
        if (httpd_resp_send_chunk(req, (const char *)buf, got) != ESP_OK) {
            return ESP_FAIL;              /* 客户端断了 */
        }
        off += got;
    }
    httpd_resp_send_chunk(req, NULL, 0);  /* 结束分块 */
    return ESP_OK;
}

/*
 * GET /ping -- 最小可用的 HTTP 存活探针。
 *
 * 不需要构造 JSON-RPC,就能确认"设备上的 HTTP 服务活着、能应答"。
 * 这比"发一个 JSON-RPC 看返回"更适合做最底层的连通性判据。
 */
static esp_err_t http_ping_get(httpd_req_t *req)
{
    /* /ping 会报设备名和 IP,一样要防 DNS rebinding */
    if (!http_guard_host_origin(req)) {
        return ESP_FAIL;
    }

    char buf[160];
    snprintf(buf, sizeof(buf),
             "{\"ok\":true,\"device\":\"%s\",\"ip\":\"%s\",\"uptime_s\":%lld,\"mcp\":\"%s\"}",
             io_cfg_device_name(), io_net_ip(),
             (long long)(esp_timer_get_time() / 1000000), IO_MCP_PROTO);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* 启动                                                                 */
/* ------------------------------------------------------------------ */

esp_err_t io_mcp_start(void)
{
    /* 单例槽 + worker task(长动作移出 httpd task,见文件头注释) */
    s_op_sem = xSemaphoreCreateBinary();
    if (!s_op_sem) {
        ESP_LOGE(TAG, "op semaphore alloc failed");
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreGive(s_op_sem);      /* 初始为空闲 */
    s_job_q = xQueueCreate(1, sizeof(io_job_t));
    if (!s_job_q) {
        ESP_LOGE(TAG, "job queue alloc failed");
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(io_job_task, "io_job", 4096, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "job task create failed");
        return ESP_ERR_NO_MEM;
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = IO_MCP_PORT;
    cfg.max_uri_handlers = 6;
    cfg.lru_purge_enable = true;
    /*
     * max_open_sockets 保持 IDF 默认(7 = LWIP_MAX_SOCKETS 10 - 服务器内部 3)。
     * **不用为并发调大** —— MCP 场景是"一个 agent 顺序调工具",不是并发负载;
     * 而每个 socket 都要占内部 RAM,设备总共只有 512 KB。
     * 之前"间歇性连接失败"的真因是**内存耗尽**(status 每次调用都 mmap
     * otadata 分区),不是连接池小。
     */
    /*
     * uri_match_fn:打开**通配 URI 模板**。
     *
     * 之前这里没设,于是 "/files/" + 星号 这种模板**根本不会展开** —— 默认的
     * 匹配器是精确比较(strncmp),只有完全相等的 URI 才命中,所以
     * GET /files/WATCH.PS1 一直 404 "Nothing matches the given URI"。
     * (之前那版代码的注释写"实测 IDF 5.5 的 httpd 对通配模板匹配不上",
     *  诊断反了:不是通配不支持,而是**没把匹配器切过去**。
     *  IDF 把 httpd_uri_match_wildcard() 作为**公开 API** 提供在
     *  esp_http_server.h 里,设进去就行。)
     *
     * 没有通配器的语义影响:模板里不带 '*'/'?' 时,这个匹配器的行为与
     * 精确比较一致(源码 httpd_uri.c: exact_match_chars == len 分支),
     * 所以 "/mcp"、"/ping" 这几条的匹配结果不变。
     */
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    /*
     * stack_size:IDF 默认 4096。httpd 是**单 task**,handler 的调用链
     * (httpd 自己的帧 + 我们的加固 + cJSON 解析/打印 + 读盘)
     * 全在这一个栈上。读一个"已存在"的文件时崩过
     * (Guru Meditation / LoadProhibited,回溯被冲垮 ⇒ 栈被写坏),
     * 给到 8192 留出余量。
     */
    cfg.stack_size = 8192;
    /*
     * recv_wait_timeout:IDF 默认 5 秒,这里压到 2 秒。
     *
     * 为什么必须压:esp_http_server 是**单 task**。任何一个"Content-Length 说
     * 很大、body 却不发完"的客户端,会让 httpd 卡在 httpd_req_recv() 里等满
     * 3×5=15 秒 —— 期间**所有**其它 agent 请求全部排队,客户端看到的是
     * "设备失联"(而 WiFi/mDNS 还活着,所以排查方向会被彻底带偏;
     * 实测就是这样把 MCP 服务器拖到看起来彻底死掉的)。
     * 正常 body 在 WiFi 上是毫秒级到达,2 秒绰绰有余;坏请求最多堵 ~4 秒。
     */
    cfg.recv_wait_timeout = 2;

    httpd_handle_t server = NULL;
    esp_err_t rc = httpd_start(&server, &cfg);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(rc));
        return rc;
    }

    static const httpd_uri_t uri_mcp = {
        .uri = "/mcp",
        .method = HTTP_POST,
        .handler = http_mcp_post,
        .user_ctx = NULL,
    };
    /* GET /mcp:规范要求要么开 SSE 流,要么回 405。我们不开流,明确回 405。 */
    static const httpd_uri_t uri_mcp_get = {
        .uri = "/mcp",
        .method = HTTP_GET,
        .handler = http_mcp_get,
        .user_ctx = NULL,
    };
    /*
     * /files/ 下的路径用**通配模板**匹配:共享盘上文件名是 8.3 短名
     * (SNAP_P.JPG 这种),数量不定(宿主随时能写新的),没法像以前那样给每个
     * 名字注册一条 URI —— 以前那 4 条 "/blob/bN" 是把**内部缓存槽位**暴露
     * 成了对外契约,现在按文件名取,通配一条就够。
     */
    static const httpd_uri_t uri_files = {
        .uri = "/files/*",
        .method = HTTP_GET,
        .handler = http_files_get,
        .user_ctx = NULL,
    };
    static const httpd_uri_t uri_ping = {
        .uri = "/ping",
        .method = HTTP_GET,
        .handler = http_ping_get,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(server, &uri_mcp);
    const esp_err_t g = httpd_register_uri_handler(server, &uri_mcp_get);
    ESP_LOGI(TAG, "GET /mcp (405 by design): %s", esp_err_to_name(g));
    const esp_err_t f = httpd_register_uri_handler(server, &uri_files);
    ESP_LOGI(TAG, "files endpoint /files/*: %s", esp_err_to_name(f));
    const esp_err_t prc = httpd_register_uri_handler(server, &uri_ping);
    ESP_LOGI(TAG, "ping endpoint /ping: %s", esp_err_to_name(prc));

    ESP_LOGI(TAG, "MCP server on port %d (POST /mcp, GET /files/*, GET /ping), protocol %s",
             IO_MCP_PORT, IO_MCP_PROTO);
    return ESP_OK;
}
