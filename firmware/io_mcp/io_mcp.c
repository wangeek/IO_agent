/*
 * io_mcp — 设备端 MCP server(Streamable HTTP + JSON-RPC 2.0)
 *
 * 拓扑:设备是服务端,agent 主动连过来(mDNS: _ioagent._tcp.local)。
 *
 * 端点:
 *   POST /mcp        JSON-RPC 2.0(MCP Streamable HTTP)
 *   GET  /blob/<id>  观察帧原字节(observe 工具回传的 URL 指向这里)
 *
 * 工具:
 *   device/info      设备身份与状态
 *   act              注入一个动作(键盘/鼠标/宏)
 *   observe          读目标机上的快照文件并回传 URL(设备不解码,只搬运)
 *   watcher/status   目标机 watcher 起没起
 *   deploy_payload   把引导内容落 flash(下次开机生效)
 *
 * ★ 安全语义(2026-10-22 事故后的定案)★
 *   - `act` **单次执行、零重试**:不因为失败就自动重发。
 *   - `act` 默认在注入前后各观察一次,把两张图的 URL 一起返回 ——
 *     **由 agent 自己判断"指令有没有生效"**,而不是靠日志里的"已发送"。
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
#include "io_net.h"
#include "io_payload.h"
#include "io_usb.h"

static const char *TAG = "io_mcp";

#define IO_MCP_PORT          8080
#define IO_MCP_PROTO         "2025-06-18"
#define IO_MCP_SERVER_NAME   "ioagent"
#define IO_MCP_SERVER_VER    "1.0.0"

/*
 * body 大小上限。
 *
 * 上限怎么定:最大的合法请求是 `deploy_payload` —— 把内容 base64 装进
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
/* 观察帧 blob 缓存                                                     */
/* ------------------------------------------------------------------ */

#define IO_BLOB_SLOTS   4
#define IO_BLOB_MAX     (256 * 1024)

typedef struct {
    char     tag[16];      /* 来自哪个文件 */
    size_t   len;
    uint32_t seq;
} io_blob_meta_t;

static uint8_t       *s_blob_mem;
static io_blob_meta_t s_blob[IO_BLOB_SLOTS];
static uint32_t       s_blob_next;    /* 环形游标 */
static uint32_t       s_blob_seq;

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
 * 好处:注入进行中,**读类工具(observe / watcher/status / device/info)
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
    return true;
}

static void op_slot_release(void)
{
    s_op_busy = false;
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

static cJSON *tool_result_text(const char *text, bool is_error)
{
    cJSON *res = cJSON_CreateObject();
    cJSON *content = cJSON_CreateArray();
    cJSON *item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "type", "text");
    cJSON_AddStringToObject(item, "text", text ? text : "");
    cJSON_AddItemToArray(content, item);
    cJSON_AddItemToObject(res, "content", content);
    if (is_error) {
        cJSON_AddBoolToObject(res, "isError", true);
    }
    return res;
}

static char *blob_url(uint32_t idx)
{
    char *buf = (char *)malloc(96);
    if (buf) {
        snprintf(buf, 96, "http://%s:%d/blob/b%u", io_net_ip(), IO_MCP_PORT, (unsigned)idx);
    }
    return buf;
}

/* 把盘上的一个文件读进 blob 缓存,返回 blob 下标(-1 = 失败)。 */
static int blob_store(const char *file, const char *tag)
{
    if (!s_blob_mem) {
        return -1;
    }
    const uint32_t idx = s_blob_next % IO_BLOB_SLOTS;
    s_blob_next++;

    uint8_t *dst = s_blob_mem + (size_t)idx * IO_BLOB_MAX;
    size_t len = 0;
    if (io_disk_read_file(file, dst, IO_BLOB_MAX, &len) != ESP_OK || len == 0) {
        return -1;
    }
    strncpy(s_blob[idx].tag, tag, sizeof(s_blob[idx].tag) - 1);
    s_blob[idx].tag[sizeof(s_blob[idx].tag) - 1] = '\0';
    s_blob[idx].len = len;
    s_blob[idx].seq = ++s_blob_seq;
    return (int)idx;
}

/* observe 一次:把快照读进 blob,返回 JSON 描述(失败时也返回 JSON,带 ok=false)。 */
static cJSON *observe_snapshot(const char *file, const char *tag)
{
    cJSON *o = cJSON_CreateObject();
    const int64_t t0 = esp_timer_get_time();
    const int idx = blob_store(file, tag);
    const int64_t dt_ms = (esp_timer_get_time() - t0) / 1000;
    /*
     * 慢读打点:读盘是从 FAT 簇链上拷到 PSRAM,正常是毫秒级。
     * 一旦超过 500 ms 就说明有异常(簇链走得太远、或者内存被换出),
     * 打出来才能在串口日志里对上"observe 卡住"这类现象。
     */
    if (dt_ms > 500) {
        ESP_LOGW(TAG, "observe %s took %lld ms", file, (long long)dt_ms);
    }
    if (idx < 0) {
        cJSON_AddBoolToObject(o, "ok", false);
        cJSON_AddStringToObject(o, "file", file);
        cJSON_AddStringToObject(o, "note",
                               "file not on the volume (is the watcher running on the host?)");
        return o;
    }
    char *url = blob_url((uint32_t)idx);
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddStringToObject(o, "file", file);
    cJSON_AddStringToObject(o, "tag", s_blob[idx].tag);
    cJSON_AddNumberToObject(o, "bytes", (double)s_blob[idx].len);
    cJSON_AddNumberToObject(o, "seq", (double)s_blob[idx].seq);
    if (url) {
        cJSON_AddStringToObject(o, "url", url);
        free(url);
    }
    return o;
}

/* ------------------------------------------------------------------ */
/* 工具:device/info                                                     */
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
 */
#define IO_LED_READY        0
#define IO_LED_BOOTING      1
#define IO_LED_NET_DOWN     2
#define IO_LED_ACT_BUSY     3
#define IO_LED_FAULT        4

static int led_state(void)
{
    if (s_op_busy) {
        return IO_LED_ACT_BUSY;      /* 动作执行中,优先级最高 */
    }
    if (!io_launcher_last_selftest_ok()) {
        return IO_LED_FAULT;         /* 上次注入自检失败 */
    }
    if (!io_net_connect_started()) {
        return IO_LED_BOOTING;       /* 还没发起过连接 */
    }
    if (!io_net_is_connected()) {
        return IO_LED_NET_DOWN;
    }
    return IO_LED_READY;
}

static cJSON *tool_device_info(void)
{
    cJSON *o = cJSON_CreateObject();
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
    cJSON_AddNumberToObject(o, "led", led_state());

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
    return o;
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
 * act 工具:单次执行、零重试(失败不自动重发)。
 *
 * 同步模型(2026-10-23):
 *   - **短动作**(tap/hotkey/move/click/scroll/短 type)在 httpd task 里直接执行,
 *     返回执行结果。
 *   - **长动作**(macro 注入类、超过 16 字符的 type)派给 **worker task**,
 *     本次请求**立刻回 accepted**;结果由 agent 回读盘上的标记文件
 *     (INJ.TXT / CAP.TXT)判定 —— 标记文件本来就是状态,不需要 job 状态机。
 *   - 设备是**单例**:同一时刻只跑一个动作。忙时再来一个动作请求 →
 *     **直接回 busy**,不排队执行。
 *   - 读类工具(observe / watcher/status / device/info)在动作执行期间**照常服务**。
 *
 * 注意:动作不再自动回传"注入前后两张截图"。异步模型下"后一张"没有意义
 * (请求早就返回了),由 agent 自己用 observe 取前后帧来比对。
 */
static cJSON *tool_act(const cJSON *args)
{
    cJSON *out = cJSON_CreateObject();

    /* 链路不可用就直接报错,不要盲发 */
    if (!io_usb_link_alive()) {
        cJSON_AddBoolToObject(out, "ok", false);
        cJSON_AddStringToObject(out, "error", "usb link is not alive (host suspended or unplugged)");
        return out;
    }
    if (!io_hid_ready()) {
        cJSON_AddBoolToObject(out, "ok", false);
        cJSON_AddStringToObject(out, "error", "HID endpoint is busy (host is not polling it)");
        return out;
    }

    const cJSON *action = cJSON_GetObjectItem(args, "action");
    if (!cJSON_IsString(action)) {
        cJSON_AddBoolToObject(out, "ok", false);
        cJSON_AddStringToObject(out, "error", "missing action");
        return out;
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
            return out;
        }
        if (strcmp(m->valuestring, "selftest") &&
            strcmp(m->valuestring, "launch_watcher") &&
            strcmp(m->valuestring, "focus_reset") &&
            strcmp(m->valuestring, "ime_toggle")) {
            cJSON_Delete(detail);
            cJSON_AddBoolToObject(out, "ok", false);
            cJSON_AddStringToObject(out, "error", "unknown macro");
            return out;
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
        return out;
    }

    if (is_macro || long_type) {
        /* ---- 长动作:派给 worker,立刻回 accepted(不等结果) ---- */
        if (xQueueSend(s_job_q, &job, 0) != pdTRUE) {
            op_slot_release();
            cJSON_Delete(detail);
            cJSON_AddBoolToObject(out, "ok", false);
            cJSON_AddStringToObject(out, "error", "job queue full");
            return out;
        }
        /* 槽由 worker 在动作跑完后释放 */
        cJSON_AddItemToObject(out, "action", detail);
        cJSON_AddBoolToObject(out, "ok", true);
        cJSON_AddBoolToObject(out, "accepted", true);
        cJSON_AddBoolToObject(out, "busy", true);
        cJSON_AddStringToObject(out, "note",
                               "accepted and running in background; poll device/info "
                               "(busy=false) then read the marker file (INJ.TXT / "
                               "CAP.TXT) to judge the outcome");
        return out;
    }

    /* ---- 短动作:同步执行 ---- */
    const esp_err_t rc = act_run(args, detail);
    op_slot_release();

    cJSON_AddItemToObject(out, "action", detail);
    cJSON_AddBoolToObject(out, "ok", (rc == ESP_OK));
    if (rc != ESP_OK) {
        cJSON_AddStringToObject(out, "error", esp_err_to_name(rc));
    }
    return out;
}

/* ------------------------------------------------------------------ */
/* 工具:observe / watcher/status / deploy_payload                        */
/* ------------------------------------------------------------------ */

static cJSON *tool_observe(const cJSON *args)
{
    const cJSON *t = cJSON_GetObjectItem(args, "target");
    const char *target = (cJSON_IsString(t) && t->valuestring[0]) ? t->valuestring : "preview";

    if (!strcmp(target, "diff")) {
        /* 帧差清单位watcher算好(哑设备原则:设备不比对),设备只搬运 */
        char buf[2048];
        size_t len = 0;
        if (io_disk_read_file("DIFF.IDX", buf, sizeof(buf) - 1, &len) != ESP_OK || len == 0) {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddBoolToObject(o, "ok", false);
            cJSON_AddStringToObject(o, "note", "DIFF.IDX not on the volume");
            return o;
        }
        buf[len] = '\0';
        cJSON *o = cJSON_CreateObject();
        cJSON_AddBoolToObject(o, "ok", true);
        cJSON_AddStringToObject(o, "file", "DIFF.IDX");
        cJSON_AddStringToObject(o, "diff", buf);
        return o;
    }

    const char *file = "SNAP_P.JPG";
    const char *tag = "preview";
    if (!strcmp(target, "full")) { file = "SNAP_A.JPG"; tag = "full"; }
    else if (!strcmp(target, "roi")) { file = "SNAP_R.JPG"; tag = "roi"; }
    else if (strcmp(target, "preview") != 0) {
        /*
         * 不是预设名 -> 当作**任意 8.3 文件名**直接读根目录。
         * 用途:读取宿主写给设备的任意文件(如槽位文件 WATCH.PS1),
         * 也是验证 find_file/read_file 真的能"找到"而不只是"报找不到"。
         */
        file = target;
        tag = "file";
    }

    return observe_snapshot(file, tag);
}

static cJSON *tool_watcher_status(void)
{
    cJSON *o = cJSON_CreateObject();
    io_file_info_t info;
    const bool found = io_disk_find_file("CAP.TXT", &info);
    cJSON_AddBoolToObject(o, "running", found);
    if (found) {
        cJSON_AddNumberToObject(o, "cap_bytes", (double)info.size);
        char head[128];
        size_t n = 0;
        if (io_disk_read_file("CAP.TXT", head, sizeof(head) - 1, &n) == ESP_OK && n) {
            head[n] = '\0';
            for (size_t i = 0; i < n; i++) {
                if (head[i] == '\r' || head[i] == '\n') { head[i] = '\0'; break; }
            }
            cJSON_AddStringToObject(o, "cap_head", head);
        }
    } else {
        cJSON_AddStringToObject(o, "note",
                               "CAP.TXT not found -- the watcher is not running on the host");
    }
    return o;
}

static cJSON *tool_deploy_payload(const cJSON *args)
{
    cJSON *o = cJSON_CreateObject();

    const cJSON *slot = cJSON_GetObjectItem(args, "slot");
    const uint32_t slot_index = cJSON_IsNumber(slot) ? (uint32_t)slot->valuedouble : 0;

    const cJSON *text = cJSON_GetObjectItem(args, "text");
    const cJSON *b64 = cJSON_GetObjectItem(args, "base64");
    if (!cJSON_IsString(text) && !cJSON_IsString(b64)) {
        cJSON_AddBoolToObject(o, "ok", false);
        cJSON_AddStringToObject(o, "error", "need either 'text' or 'base64'");
        return o;
    }

    const io_slot_desc_t *sd = io_disk_slot_at(slot_index);
    if (!sd || sd->kind != IO_SLOT_KIND_SLOT) {
        cJSON_AddBoolToObject(o, "ok", false);
        cJSON_AddStringToObject(o, "error", "no such slot");
        return o;
    }

    uint8_t *buf = (uint8_t *)heap_caps_malloc(sd->capacity, MALLOC_CAP_SPIRAM);
    if (!buf) {
        cJSON_AddBoolToObject(o, "ok", false);
        cJSON_AddStringToObject(o, "error", "out of memory");
        return o;
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
            return o;
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
        return o;
    }
    const esp_err_t rc = io_payload_store(slot_index, buf, len);
    op_slot_release();
    heap_caps_free(buf);

    cJSON_AddBoolToObject(o, "ok", (rc == ESP_OK));
    cJSON_AddStringToObject(o, "slot", sd->name);
    cJSON_AddNumberToObject(o, "len", (double)len);
    if (rc != ESP_OK) {
        cJSON_AddStringToObject(o, "error", esp_err_to_name(rc));
        return o;
    }
    cJSON_AddNumberToObject(o, "payload_seq", (double)io_payload_active_seq());
    cJSON_AddStringToObject(o, "note",
                           "stored in flash only -- the RAM disk copy is written at next boot");
    return o;
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
static void add_tool(cJSON *arr, const char *name, const char *desc, cJSON *props,
                     const char *required)
{
    cJSON *t = cJSON_CreateObject();
    cJSON_AddStringToObject(t, "name", name);
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
}

static cJSON *build_tools_list(void)
{
    cJSON *tools = cJSON_CreateArray();

    {
        cJSON *p = cJSON_CreateObject();
        add_tool(tools, "device/info",
                 "Device identity, link state, slot table and payload sequence.",
                 p, NULL);
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
        add_tool(tools, "act",
                 "Inject ONE action on the host. Single execution, never retried. "
                 "Long actions (macro / long text) are accepted and run in background: "
                 "the reply is {\"accepted\":true,\"busy\":true} and the outcome is "
                 "judged by the marker file (INJ.TXT / CAP.TXT) -- poll device/info "
                 "until busy=false, then observe the marker. Short actions run inline. "
                 "If another action is already running the reply is {\"busy\":true} "
                 "(the device is a singleton; it never queues actions).",
                 p, "action");
    }
    {
        cJSON *p = cJSON_CreateObject();
        cJSON_AddItemToObject(p, "target", tool_schema_str(
            "preview | full | roi | diff, or any 8.3 filename on the volume "
            "(e.g. INJ.TXT, CAP.TXT) to read that file back"));
        add_tool(tools, "observe",
                 "Read a snapshot file from the host's RAM disk and return a URL for "
                 "the raw bytes. The device never decodes or compares images.",
                 p, NULL);
    }
    {
        cJSON *p = cJSON_CreateObject();
        add_tool(tools, "watcher/status",
                 "Whether the watcher on the host is running (CAP.TXT present).",
                 p, NULL);
    }
    {
        cJSON *p = cJSON_CreateObject();
        cJSON_AddItemToObject(p, "slot", tool_schema_num("slot index, 0=WATCH.PS1 1=AGENT.SH 2=AGENT.EXE"));
        cJSON_AddItemToObject(p, "text", tool_schema_str("plain text payload"));
        cJSON_AddItemToObject(p, "base64", tool_schema_str("base64 payload (alternative to text)"));
        add_tool(tools, "deploy_payload",
                 "Store a payload into the flash A/B bank. Takes effect at the next boot "
                 "(the RAM disk copy is written by the boot-time overlay).",
                 p, "slot");
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

/* Host / Origin 检查(GET 端点也要 —— /blob 装的是桌面帧) */
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

static cJSON *dispatch_rpc(const cJSON *req)
{
    const cJSON *method = cJSON_GetObjectItem(req, "method");
    const cJSON *id = cJSON_GetObjectItem(req, "id");
    if (!cJSON_IsString(method)) {
        return NULL;
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
            return NULL;
        }
        if (!cJSON_IsObject(targs)) {
            targs = NULL;
        }
        const char *n = tname->valuestring;
        cJSON *payload = NULL;
        bool err = false;
        if (!strcmp(n, "device/info")) {
            payload = tool_device_info();
        } else if (!strcmp(n, "act")) {
            payload = tool_act(targs);
            err = !cJSON_IsTrue(cJSON_GetObjectItem(payload, "ok"));
        } else if (!strcmp(n, "observe")) {
            payload = tool_observe(targs);
        } else if (!strcmp(n, "watcher/status")) {
            payload = tool_watcher_status();
        } else if (!strcmp(n, "deploy_payload")) {
            payload = tool_deploy_payload(targs);
            err = !cJSON_IsTrue(cJSON_GetObjectItem(payload, "ok"));
        } else {
            char msg[128];
            snprintf(msg, sizeof(msg), "unknown tool: %s", n);
            result = cJSON_CreateObject();
            cJSON *content = cJSON_CreateArray();
            cJSON *item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "type", "text");
            cJSON_AddStringToObject(item, "text", msg);
            cJSON_AddItemToArray(content, item);
            cJSON_AddItemToObject(result, "content", content);
            cJSON_AddBoolToObject(result, "isError", true);
        }
        if (payload) {
            char *txt = cJSON_PrintUnformatted(payload);
            result = cJSON_CreateObject();
            cJSON *content = cJSON_CreateArray();
            cJSON *item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "type", "text");
            cJSON_AddStringToObject(item, "text", txt ? txt : "{}");
            cJSON_AddItemToArray(content, item);
            cJSON_AddItemToObject(result, "content", content);
            if (err) {
                cJSON_AddBoolToObject(result, "isError", true);
            }
            cJSON_Delete(payload);
            free(txt);
        }
    } else {
        return NULL;   /* 未知方法 -> 交给调用方报 method not found */
    }

    (void)id;
    return result;
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
            const cJSON *id = cJSON_GetObjectItem(one, "id");
            if (cJSON_IsString(method) && !strcmp(method->valuestring, "notifications/initialized")) {
                continue;   /* 通知,无响应 */
            }
            cJSON *res = dispatch_rpc(one);
            if (res) {
                cJSON_AddItemToObject(res, "jsonrpc", cJSON_CreateString("2.0"));
                cJSON_AddItemToArray(arr, res);
            } else {
                cJSON *err = rpc_error(id, -32601, "method not found");
                cJSON_AddItemToArray(arr, err);
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
        const cJSON *id = cJSON_GetObjectItem(req_json, "id");
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
            cJSON *out = rpc_ok(id, res);
            resp = cJSON_PrintUnformatted(out);
            cJSON_Delete(out);
        } else {
            cJSON *out = rpc_error(id, -32601, "method not found");
            resp = cJSON_PrintUnformatted(out);
            cJSON_Delete(out);
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

static esp_err_t http_blob_get(httpd_req_t *req)
{
    /* /blob 装的是目标机桌面帧,一样要防 DNS rebinding */
    if (!http_guard_host_origin(req)) {
        return ESP_FAIL;
    }

    const char *tail = req->uri + strlen("/blob/");
    if (!*tail) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad blob id");
        return ESP_FAIL;
    }
    /* URL 形式是 "/blob/b<n>" —— 跳过可选的 'b' 前缀再按十进制解析。
       (踩过:URL 生成用 "b0"、解析按纯数字,于是每个 blob 请求都 400。) */
    if (*tail == 'b' || *tail == 'B') {
        tail++;
    }
    char *end = NULL;
    const long idx = strtol(tail, &end, 10);
    if (end == tail || idx < 0 || idx >= IO_BLOB_SLOTS) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "bad blob id");
        return ESP_FAIL;
    }
    if (!s_blob_mem || s_blob[idx].len == 0) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "blob expired");
        return ESP_FAIL;
    }
    const char *ctype = "image/jpeg";
    if (!strcmp(s_blob[idx].tag, "diff")) {
        ctype = "application/json";
    }
    httpd_resp_set_type(req, ctype);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, (const char *)s_blob_mem + (size_t)idx * IO_BLOB_MAX,
                    s_blob[idx].len);
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
    if (!s_blob_mem) {
        s_blob_mem = (uint8_t *)heap_caps_malloc((size_t)IO_BLOB_SLOTS * IO_BLOB_MAX,
                                                 MALLOC_CAP_SPIRAM);
        if (!s_blob_mem) {
            ESP_LOGE(TAG, "blob cache alloc failed");
            return ESP_ERR_NO_MEM;
        }
    }

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
     * stack_size:IDF 默认 4096。httpd 是**单 task**,handler 的调用链
     * (httpd 自己的帧 + 我们的加固 + cJSON 解析/打印 + observe 的读盘)
     * 全在这一个栈上。observe 读一个"已存在"的文件时崩过
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
    /*
     * blob 端点用**精确路径**而不是通配(模板是 "/blob/" 加一个星号):
     * 实测 IDF 5.5 的 httpd 对那个通配模板没有匹配上 "/blob/b0"
     * (返回 404 "Nothing matches the given URI"),而 "/ping" 正常。
     * 4 个固定槽位 -> 注册 4 条精确 URI,行为确定。
     */
    static const httpd_uri_t uri_blob[IO_BLOB_SLOTS] = {
        { .uri = "/blob/b0", .method = HTTP_GET, .handler = http_blob_get, .user_ctx = NULL },
        { .uri = "/blob/b1", .method = HTTP_GET, .handler = http_blob_get, .user_ctx = NULL },
        { .uri = "/blob/b2", .method = HTTP_GET, .handler = http_blob_get, .user_ctx = NULL },
        { .uri = "/blob/b3", .method = HTTP_GET, .handler = http_blob_get, .user_ctx = NULL },
    };
    static const httpd_uri_t uri_ping = {
        .uri = "/ping",
        .method = HTTP_GET,
        .handler = http_ping_get,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(server, &uri_mcp);
    for (int i = 0; i < IO_BLOB_SLOTS; i++) {
        const esp_err_t rc = httpd_register_uri_handler(server, &uri_blob[i]);
        ESP_LOGI(TAG, "blob endpoint /blob/b%d: %s", i, esp_err_to_name(rc));
    }
    const esp_err_t prc = httpd_register_uri_handler(server, &uri_ping);
    ESP_LOGI(TAG, "ping endpoint /ping: %s", esp_err_to_name(prc));

    ESP_LOGI(TAG, "MCP server on port %d (POST /mcp, GET /blob/*), protocol %s",
             IO_MCP_PORT, IO_MCP_PROTO);
    return ESP_OK;
}
