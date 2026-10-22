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

/* 每个字符的按下保持时长。命令约 150 字符 -> 全程约 1.2 s(远小于 10ms/步)。 */
#define TYPE_PER_CHAR_MS   8
/* 打完命令后等一下再回车(让 Run 框把这一长串处理完) */
#define TYPE_SETTLE_MS     400
/* CAP.TXT 轮询:2s 间隔,最多 5 次 */
#define CAP_POLL_MS        2000
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
 * focus_reset 宏(N4):
 *   a) Shift 点切 —— IME 归零。中文输入法开着时,后面注入的字母会被
 *      吃掉变成拼音(头号杀手)。注意这是 best-effort:输入法若没勾
 *      "Shift 切换中英文"就不生效;Win+R 的 Run 框本身恒为英文输入。
 *   b) Win+M —— 最小化所有窗口,保证桌面上没有遮挡。
 *   c) Win+R —— 打开运行框。
 */
static esp_err_t focus_reset(void)
{
    ESP_RETURN_ON_ERROR(io_hid_keyboard_tap(IO_HID_KEY_LEFTSHIFT, 30),
                        TAG, "Shift 点切失败");
    vTaskDelay(pdMS_TO_TICKS(MACRO_GAP_MS));

    ESP_RETURN_ON_ERROR(io_hid_hotkey(IO_HID_MOD_LGUI, 'M', 30),
                        TAG, "Win+M 失败");
    vTaskDelay(pdMS_TO_TICKS(MACRO_GAP_MS));

    ESP_RETURN_ON_ERROR(io_hid_hotkey(IO_HID_MOD_LGUI, 'R', 30),
                        TAG, "Win+R 失败");
    vTaskDelay(pdMS_TO_TICKS(MACRO_GAP_MS));
    return ESP_OK;
}

esp_err_t io_launcher_focus_reset(void)
{
    return focus_reset();
}

esp_err_t io_launcher_inject_watch(void)
{
    return io_hid_type_text(LAUNCH_CMD, TYPE_PER_CHAR_MS);
}

/* 注入一条命令:宏 → 打字 → 回车。 */
static esp_err_t inject_command(const char *cmd)
{
    ESP_RETURN_ON_ERROR(focus_reset(), TAG, "focus_reset 宏失败");
    ESP_RETURN_ON_ERROR(io_hid_type_text(cmd, TYPE_PER_CHAR_MS), TAG, "注入命令失败");
    vTaskDelay(pdMS_TO_TICKS(TYPE_SETTLE_MS));
    return io_hid_keyboard_tap(IO_HID_KEY_ENTER, 30);
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
    if (inject_command(LAUNCH_CMD) != ESP_OK) {
        ESP_LOGE(TAG, "injecting the launch command failed");
        return false;
    }

    for (int i = 0; i < CAP_POLL_TRIES; i++) {
        vTaskDelay(pdMS_TO_TICKS(CAP_POLL_MS));
        if (cap_present()) {
            io_file_info_t info;
            io_disk_find_file(CAP_FILE, &info);
            ESP_LOGI(TAG, "watcher is up (%s size=%u)", CAP_FILE, (unsigned)info.size);
            return true;
        }
        ESP_LOGW(TAG, "waiting for %s (%d/%d)", CAP_FILE, i + 1, CAP_POLL_TRIES);
    }
    ESP_LOGE(TAG, "watcher did not come up after %d tries", CAP_POLL_TRIES);
    return false;
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
