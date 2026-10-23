#include "io_launcher.h"

#include <string.h>
#include <stdbool.h>

#include <esp_log.h>
#include <esp_check.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "io_disk.h"
#include "io_hid.h"
#include "io_usb.h"

static const char *TAG = "io_launcher";

/* 目标机上的能力自陈文件:watcher 起来就会写它(见 watcher/watch.ps1) */
#define CAP_FILE          "CAP.TXT"

/* 注入自检的标记文件 */
#define INJ_FILE          "INJ.TXT"

/*
 * 注入自检命令:让**目标机**把一个标记写回盘上。
 *
 * 这是分层判据,把两件事分开:
 *   INJ.TXT 出现        -> HID 注入通了、Run 框开了、命令执行了
 *   INJ.TXT 不出现      -> 注入链路本身不通(还没到 watcher 那一层)
 *   INJ.TXT 出现但
 *   CAP.TXT 不出现      -> 注入通了,问题在 watcher 侧
 *
 * 标记文件留在 RAM disk 里,断电即失(无痕);它也不影响 watcher(后者只认
 * 自己的固定文件名)。
 */
static const char *const SELFTEST_CMD =
    "cmd /c for %d in (I O A G E N T) do @if exist %d:\\ echo INJECT_OK > %d:\\INJ.TXT";

/*
 * 按**卷标**解析的启动命令。禁止写死盘符: dongle 拔线即整机断电,
 * 重入是冷启动,Windows 可能分配到别的盘符 -> 写死 X:\ 的命令在重入后必然失效。
 * 命令按卷标逐个试候选盘符,if exist 命中即启动。
 */
static const char *const LAUNCH_CMD =
    "cmd /c for %d in (I O A G E N T) do @if exist %d:\\WATCH.PS1 "
    "start \"\" powershell -ExecutionPolicy Bypass -WindowStyle Hidden %d:\\WATCH.PS1";

/* 每个字符的按下保持时长。IME 需要时间处理每个键:8 ms 时微软拼音会把输入
 * 打成乱码(实测 "ni hao " 变成 49 个"啊"),60 ms 正常。 */
#define TYPE_PER_CHAR_MS   IO_HID_KEY_HOLD_MS
/* 打完命令后等一下再回车(让 Run 框把这一长串处理完) */
#define TYPE_SETTLE_MS     400
/* CAP.TXT 轮询:1s 间隔,最多 5 次(watcher 一起来就写 CAP.TXT,通常 1s 内就有) */
#define CAP_POLL_MS        1000
#define CAP_POLL_TRIES     5
/* 注入自检轮询:1s 间隔,最多 4 次 */
#define INJ_POLL_MS        1000
#define INJ_POLL_TRIES     4
/* 宏各步之间的间隔 */
#define MACRO_GAP_MS       500
/* 一轮失败后到下一轮的间隔 */
#define RETRY_GAP_MS       3000

/* ------------------------------------------------------------------ */

static bool cap_present(void)
{
    io_file_info_t info;
    return io_disk_find_file(CAP_FILE, &info);
}

/*
 * focus_reset 宏:把目标机摆成"能安全接收一条英文命令"的状态。
 *
 * 顺序与依据(2026-10-23 实测标定):
 *   a) Win+M       —— 最小化所有窗口,保证桌面上没有遮挡。
 *   b) Win+R       —— 打开运行框。
 *   c) Ctrl+Space  —— 微软拼音的"开/关输入法"。
 *   d) Shift       —— 微软拼音的"中/英文切换"。
 *
 * 为什么 IME 要两步:Shift 是**状态相关**的 toggle(中->英 或 英->中),
 * 单按一次的结果取决于注入前是什么态 —— 这正是之前"命令有时被拼音吃掉、
 * 有时又正常"的根因。Ctrl+Space 先把输入法关掉(或打开),再按 Shift,
 * 终点一定是纯 ASCII 态:
 *   开+中文 --Ctrl+Space--> 关     --Shift--> 关(纯 ASCII)
 *   开+英文 --Ctrl+Space--> 关     --Shift--> 关(纯 ASCII)
 *   关     --Ctrl+Space--> 开+中文 --Shift--> 开+英文(纯 ASCII)
 * 三个分支都在目标机上逐个实测过(探针 "ni hao " 回读为纯 ASCII)。
 *
 * 顺序还有讲究:IME 的两个键必须在**运行框已经有焦点之后**再按,
 * 否则作用的是上一个前台窗口的 IME 上下文。
 */
static esp_err_t focus_reset(void)
{
    /*
     * 注意:keycode 必须是 **HID 用法 ID**,不能直接传 ASCII 字符。
     * 之前这里传的是 'M'/'R'/'A',于是 Win+M / Win+R / Ctrl+A 实际按的是
     * 小键盘上的键 —— 组合键从来没生效过(表现就是"运行框不打开")。
     */
    uint8_t key_m = 0, key_r = 0, key_a = 0;
    io_hid_keycode_for_letter('M', &key_m);
    io_hid_keycode_for_letter('R', &key_r);
    io_hid_keycode_for_letter('A', &key_a);

    ESP_RETURN_ON_ERROR(io_hid_hotkey(IO_HID_MOD_LGUI, key_m, IO_HID_CHORD_HOLD_MS),
                        TAG, "Win+M 失败");
    vTaskDelay(pdMS_TO_TICKS(MACRO_GAP_MS));

    ESP_RETURN_ON_ERROR(io_hid_hotkey(IO_HID_MOD_LGUI, key_r, IO_HID_CHORD_HOLD_MS),
                        TAG, "Win+R 失败");
    vTaskDelay(pdMS_TO_TICKS(MACRO_GAP_MS));

    ESP_RETURN_ON_ERROR(io_hid_hotkey(IO_HID_MOD_LCTRL, IO_HID_KEY_SPACE,
                                      IO_HID_CHORD_HOLD_MS),
                        TAG, "Ctrl+Space 失败");
    vTaskDelay(pdMS_TO_TICKS(MACRO_GAP_MS));

    ESP_RETURN_ON_ERROR(io_hid_keyboard_tap(IO_HID_KEY_LEFTSHIFT, IO_HID_KEY_HOLD_MS),
                        TAG, "Shift 点切失败");
    vTaskDelay(pdMS_TO_TICKS(MACRO_GAP_MS));

    /*
     * Ctrl+A:把运行框里**历史预填**的上一条命令全选掉。
     *
     * 不做这一步的后果很隐蔽:运行框会记住上次输入并**预填**(实测预填了
     * "regedit"),而预填的文字不是选中态 —— 我们打的命令会被**追加**在它
     * 后面,变成 "regeditcmd /c for %d ..." 这种废命令,执行后什么也不会发生,
     * 看起来就像"注入没生效"。全选之后,接着打的字符会整段替换掉预填内容。
     */
    ESP_RETURN_ON_ERROR(io_hid_hotkey(IO_HID_MOD_LCTRL, key_a, IO_HID_CHORD_HOLD_MS),
                        TAG, "Ctrl+A 失败");
    vTaskDelay(pdMS_TO_TICKS(MACRO_GAP_MS));
    return ESP_OK;
}

esp_err_t io_launcher_focus_reset(void)
{
    return focus_reset();
}

/* 注入一条命令:宏 → 打字 → 回车。 */
static esp_err_t inject_command(const char *cmd)
{
    ESP_RETURN_ON_ERROR(focus_reset(), TAG, "focus_reset 宏失败");
    ESP_RETURN_ON_ERROR(io_hid_type_text(cmd, TYPE_PER_CHAR_MS), TAG, "注入命令失败");
    vTaskDelay(pdMS_TO_TICKS(TYPE_SETTLE_MS));
    return io_hid_keyboard_tap(IO_HID_KEY_ENTER, IO_HID_KEY_HOLD_MS);
}

/* 注入自检:确认 HID 注入链路真的到达了目标机。 */
static bool inject_selftest(void)
{
    ESP_LOGI(TAG, "injection self-test: typing the marker command ...");
    if (inject_command(SELFTEST_CMD) != ESP_OK) {
        ESP_LOGE(TAG, "injection self-test: could not type the command");
        return false;
    }
    for (int i = 0; i < INJ_POLL_TRIES; i++) {
        vTaskDelay(pdMS_TO_TICKS(INJ_POLL_MS));
        if (io_disk_find_file(INJ_FILE, NULL)) {
            ESP_LOGI(TAG, "injection self-test PASSED (%s appeared)", INJ_FILE);
            return true;
        }
    }
    ESP_LOGE(TAG, "injection self-test FAILED (%s never appeared) -- "
             "HID injection did not reach the host", INJ_FILE);
    return false;
}

/*
 * 注入自检(远程入口):打一条命令让**目标机**把标记写回盘上。
 * 成功 = INJ.TXT 出现 = HID 注入链路真的到达了目标机。
 *
 * 为什么要自检:设备自己看不到目标机屏幕,"命令发出去了"不等于
 * "命令在目标机上执行了"。INJ.TXT 是唯一能客观区分这两件事的判据。
 */
esp_err_t io_launcher_run_selftest(void)
{
    return inject_selftest() ? ESP_OK : ESP_ERR_TIMEOUT;
}

/*
 * 拉起 watcher(远程入口):注入启动命令 → 等 CAP.TXT 出现。
 * CAP.TXT 是 watcher 一启动就写的,正常 1s 内就有。
 */
esp_err_t io_launcher_run_launch(void)
{
    ESP_RETURN_ON_ERROR(inject_command(LAUNCH_CMD), TAG, "注入启动命令失败");

    for (int i = 0; i < CAP_POLL_TRIES; i++) {
        vTaskDelay(pdMS_TO_TICKS(CAP_POLL_MS));
        if (cap_present()) {
            io_file_info_t info;
            io_disk_find_file(CAP_FILE, &info);
            ESP_LOGI(TAG, "watcher is up (%s size=%u)", CAP_FILE, (unsigned)info.size);
            return ESP_OK;
        }
        ESP_LOGW(TAG, "waiting for %s (%d/%d)", CAP_FILE, i + 1, CAP_POLL_TRIES);
    }
    ESP_LOGE(TAG, "watcher did not come up after %d tries", CAP_POLL_TRIES);
    return ESP_ERR_TIMEOUT;
}

/* 一轮:自检 → 命令 → 等 CAP.TXT。 */
static bool try_launch_once(void)
{
    io_hid_ep_probe("before-inject");
    /* 阶段 1:先自检注入链路。不通就别去拉起 watcher(免得白敲键盘) */
    if (!inject_selftest()) {
        return false;
    }

    /* 阶段 2:真正的启动命令 */
    ESP_LOGI(TAG, "launching watcher on the host ...");
    return io_launcher_run_launch() == ESP_OK;
}

static void launcher_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "launcher started (autostart=%d)", IO_LAUNCHER_AUTOSTART);

    for (;;) {
        /* 1) 等 USB 链路可用 */
        while (!io_usb_link_alive() || !io_usb_host_configured()) {
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        /* HID 端点要真的能发(端点忙不算故障,等就行) */
        int probe = 0;
        while (!io_hid_wait_ready(200)) {
            if (probe++ == 0) {
                io_hid_ep_probe("wait-ready-loop");
            }
            vTaskDelay(pdMS_TO_TICKS(200));
        }

        /* 2) 目标机已经有 watcher 了吗?有就别打扰 */
        if (cap_present()) {
            ESP_LOGI(TAG, "%s already present -- watcher already running", CAP_FILE);
            vTaskDelete(NULL);
            return;
        }

        /* 3) 拉起 */
        if (try_launch_once()) {
            vTaskDelete(NULL);
            return;
        }

        /* 4) 失败:退避后重来一轮 */
        vTaskDelay(pdMS_TO_TICKS(RETRY_GAP_MS));
    }
}

esp_err_t io_launcher_start(void)
{
#if IO_LAUNCHER_AUTOSTART
    if (xTaskCreate(launcher_task, "launcher", 4096, NULL, 4, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
#else
    ESP_LOGI(TAG, "autostart disabled -- launcher not started");
    return ESP_OK;
#endif
}
