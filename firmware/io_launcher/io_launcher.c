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
 *   b) Win+R       —— 打开运行框(**新开的框 = 输入态的默认态 = 中文**)。
 *   c) Ctrl+A      —— 全选运行框里**历史预填**的上一条命令。
 *
 * ⚠ 这里**不按 Shift**:切输入法态是可选的一步,由 inject_command_ime() 在
 *   **打完这个序列之后、打字之前**单独按(盲切,50/50),或者由调用方先用
 *   探针测出当前态、确定不需要切。焦点必须落在"接收这次打字的那个框"上,
 *   所以 Shift 必须在 Win+R 之后 —— 但它是后加的一步,不是这个序列的一环。
 *   (注释早前写成"Win+M → Win+R → Shift → Ctrl+A",与代码不符,已订正。)
 *
 * 关于 IME(踩过的坑,都记在这儿):
 *   - 输入态是**每个输入框上下文各自一份**;新开的运行框 = 默认态(本机是中文)。
 *     ⇒ "新框 + 单次 Shift"是**确定性**的(默认中文 → 英文)。
 *   - Shift 是 toggle:作用在**已经获得焦点**的上下文上 ⇒ 必须放在 Win+R 之后。
 *   - "Ctrl+Space + Shift"没用:两个键在微软拼音里是同一个开关,按两次等于没切
 *     (而且 Ctrl+Space 无论同报告还是先报修饰键都实测无效)。
 *   - 复用同一个旧运行框时,Shift 就是盲切 ⇒ 结果取决于上一轮(实测 OK/FAIL 交替)。
 *     所以每次注入都**新开一个框**(focus_reset 里的 Win+R 会新开 ✅)。
 *
 * ⚠️ 注入是破坏性操作:失败**不自动重试**(见 io_launcher_run_launch)。
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

static esp_err_t inject_command_ime(const char *cmd, bool shift_ime);

/* 注入一条命令:宏 → 打字 → 回车。 */
static esp_err_t inject_command(const char *cmd)
{
    return inject_command_ime(cmd, false);
}

/*
 * 同上,但可以在**打字之前**先单按一次 Shift(切输入法态)。
 *
 * 关键:Shift 必须作用在**接收这次打字的那个运行框**的上下文上 ——
 * 输入态是每个输入框上下文各自一份,而 Win+R 每次都新开一个框。
 * 所以顺序必须是:开框(Win+R)→ Shift → 打字,不能先 Shift 再开框。
 */
static esp_err_t inject_command_ime(const char *cmd, bool shift_ime)
{
    ESP_RETURN_ON_ERROR(focus_reset(), TAG, "focus_reset 宏失败");
    if (shift_ime) {
        ESP_RETURN_ON_ERROR(io_hid_keyboard_tap(IO_HID_KEY_LEFTSHIFT, IO_HID_KEY_HOLD_MS),
                            TAG, "Shift 点切失败");
        vTaskDelay(pdMS_TO_TICKS(MACRO_GAP_MS));
    }
    ESP_RETURN_ON_ERROR(io_hid_type_text(cmd, TYPE_PER_CHAR_MS), TAG, "注入命令失败");
    vTaskDelay(pdMS_TO_TICKS(TYPE_SETTLE_MS));
    return io_hid_keyboard_tap(IO_HID_KEY_ENTER, IO_HID_KEY_HOLD_MS);
}

/*
 * 只有"设备自己能测出来"的状态才配 LED 语义(见 README 决策表):
 * 这里记的是**注入自检有没有通过** —— 自检失败意味着"手不灵",
 * 是现场最需要被看见的一类故障。
 */
static bool s_last_selftest_ok = true;

bool io_launcher_last_selftest_ok(void)
{
    return s_last_selftest_ok;
}

/*
 * 注入自检:确认 HID 注入链路真的到达了目标机。
 *
 * ★ 判据必须是"**这次**写的",不能只是"盘上有" ★
 *   INJ.TXT 留在 RAM disk 里(断电即失,但同一次开机内一直在),上一次
 *   成功留下的那个还在。**只判存在性** ⇒ 第二次自检会在 1 秒后直接报
 *   PASSED,哪怕这次注入根本没到达目标机。
 *   这正好打在"IME 态探针"上:探针的职责是判定当前是中文态还是英文态,
 *   判错 ⇒ 后面打的全是乱码,而且**看起来一切正常**。
 *
 *   所以:注入前记下 INJ.TXT 的最后写入时间,要求它**变了**才算数。
 *   (FAT 时间戳 2 秒粒度;一次自检本身要 ~7 秒,两次自检不可能落进
 *    同一个 2 秒桶 ⇒ 不会误判。若目标机没写过这文件,则是"从无到有",
 *    同样算数。)
 */
static bool inject_selftest(void)
{
    /* 注入前的基线:文件在不在、在的话它当时的时间戳是多少 */
    io_file_info_t before;
    const bool had_before = io_disk_find_file(INJ_FILE, &before);

    ESP_LOGI(TAG, "injection self-test: typing the marker command ...");
    if (inject_command(SELFTEST_CMD) != ESP_OK) {
        ESP_LOGE(TAG, "injection self-test: could not type the command");
        s_last_selftest_ok = false;
        return false;
    }
    for (int i = 0; i < INJ_POLL_TRIES; i++) {
        vTaskDelay(pdMS_TO_TICKS(INJ_POLL_MS));
        io_file_info_t now;
        if (!io_disk_find_file(INJ_FILE, &now)) {
            continue;                          /* 还没出现 */
        }
        if (had_before && now.mtime == before.mtime) {
            continue;                          /* 还是上一次留下的那个,不算数 */
        }
        ESP_LOGI(TAG, "injection self-test PASSED (%s refreshed, mtime %lld)",
                 INJ_FILE, (long long)now.mtime);
        s_last_selftest_ok = true;
        return true;
    }
    ESP_LOGE(TAG, "injection self-test FAILED (%s never refreshed) -- "
             "HID injection did not reach the host", INJ_FILE);
    s_last_selftest_ok = false;
    return false;
}

/* 注入自检 = **IME 态探针**。
 *
 * INJ.TXT 出现 = 刚才那条命令是**纯 ASCII** 打进去并跑起来了
 * (命令里含 % \ > @ ( ) 等符号,中文态下必然被打成中英混排 → 跑不起来)。
 * INJ.TXT 不出现 = 当时 IME 在中文态,命令被打残了。
 *
 * 供远程入口复用。**单次执行,内部零重试**。
 */
esp_err_t io_launcher_run_selftest(void)
{
    return inject_selftest() ? ESP_OK : ESP_ERR_TIMEOUT;
}

/*
 * 单独按一次 Shift = 微软拼音的"中/英文切换"(toggle)。
 *
 * 只在**知道当前是中文态**时用(典型场景:探针失败之后)。它不输入任何字符,
 * 所以即使目标机在锁屏也只会切输入法态,不会往 PIN 输入框里塞东西。
 */
esp_err_t io_launcher_ime_toggle(void)
{
    ESP_RETURN_ON_ERROR(focus_reset(), TAG, "focus_reset 失败");
    return io_hid_keyboard_tap(IO_HID_KEY_LEFTSHIFT, IO_HID_KEY_HOLD_MS);
}

/*
 * 拉起 watcher(远程入口)。
 *
 * 流程:**探针 → 启动命令**。
 *   1. 先打一次自检当探针。INJ.TXT 出现 = IME 当时在英文态 ✅。
 *   2. 探针通过 → 再打启动命令(这时的 IME 状态已知是英文态)→ 等 CAP.TXT。
 *   3. 探针没过 → **不自动重试**(注入是破坏性操作,重试=持续破坏),
 *      返回错误并说明原因,由调用方决定要不要补一次 ime_toggle 再来。
 *
 * @param ime_toggle  true = 探针之前先按一次 Shift 切输入法态(盲切,50/50)
 */
esp_err_t io_launcher_run_launch(bool ime_toggle)
{
    /* 已经在跑就别重复拉起:两个 watcher 会抢同一个卷(写同一批文件)。 */
    if (cap_present()) {
        ESP_LOGI(TAG, "watcher already running -- not launching another one");
        return ESP_OK;
    }

    /* 一条命令 = 开框 → (可选)Shift 切输入法态 → 打字 → 回车。
     * Shift 必须落在**接收这次打字的那个框**上(输入态是每个框一份)。 */
    ESP_RETURN_ON_ERROR(inject_command_ime(LAUNCH_CMD, ime_toggle),
                        TAG, "注入启动命令失败");

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
    /* 阶段 1:自检(= IME 态探针)。不通就别去拉起 watcher(免得白敲键盘) */
    if (!inject_selftest()) {
        return false;
    }

    /* 阶段 2:真正的启动命令(探针过了,此时 IME 已知在英文态) */
    ESP_LOGI(TAG, "launching watcher on the host ...");
    ESP_RETURN_ON_ERROR(inject_command(LAUNCH_CMD), TAG, "注入启动命令失败");
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
