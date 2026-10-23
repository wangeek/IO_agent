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
    const int idx = blob_store(file, tag);
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
        cJSON_AddItemToObject(p, "target", tool_schema_str("preview | full | roi | diff"));
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
        result = cJSON_CreateObject();
        cJSON_AddStringToObject(result, "protocolVersion", IO_MCP_PROTO);
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
    char *body = NULL;
    size_t len = 0;
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
                if (r == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 3) {
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
        len = (size_t)got;
    }

    cJSON *req_json = body ? cJSON_Parse(body) : NULL;
    free(body);
    if (!req_json) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
        return ESP_FAIL;
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
        resp = cJSON_PrintUnformatted(arr);
        cJSON_Delete(arr);
    } else {
        const cJSON *method = cJSON_GetObjectItem(req_json, "method");
        const cJSON *id = cJSON_GetObjectItem(req_json, "id");
        cJSON *res = NULL;
        if (cJSON_IsString(method) && !strcmp(method->valuestring, "notifications/initialized")) {
            httpd_resp_set_hdr(req, "Mcp-Session-Id", "ioagent-1");
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
    httpd_resp_set_hdr(req, "Mcp-Session-Id", "ioagent-1");
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
