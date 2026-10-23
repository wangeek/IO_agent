/*
 * io_launcher — M2:把目标机上的 watcher 拉起来(设备端的"手")
 *
 * 为什么要它:watch.ps1 跑在**目标机**上(零安装),但它必须由设备来启动
 * ——设备是 agent 唯一的物理入口。所以设备要能"在目标机上敲键盘"。
 *
 * 链路:
 *   1. 等 USB 链路可用(枚举完成 + HID 端点能发)
 *   2. 看 CAP.TXT 在不在 —— 在 = 目标机已经有 watcher 在跑,别打扰
 *   3. focus_reset 宏:Shift 点切(IME 归零)→ Win+M → Win+R
 *   4. 打字:按**卷标**解析的启动命令(不写死盘符)
 *   5. 回车 → 轮询 CAP.TXT(2s × 5 次)判定拉起成功
 *
 * 安全基线:
 *   - 每次注入结束都回到键盘基线(无残留按键)
 *   - 鼠标 clamp ±127(在 io_hid 里)
 *   - 检测到人动键盘即中止 —— 属于 watcher / MCP 侧,不在本模块
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 自动拉起开关。
 *
 * 默认 1 = 设备一上电就把目标机的 watcher 拉起来。
 * **量产必须提供物理或 NVS 开关**:自动注入会打断目标机上正在做的事。
 * 改这个值需要改 firmware/io_launcher/CMakeLists.txt 的编译定义。
 */
#ifndef IO_LAUNCHER_AUTOSTART
#define IO_LAUNCHER_AUTOSTART 1
#endif

/** 启动拉起任务(自动拉起开关关闭时什么都不做)。 */
esp_err_t io_launcher_start(void);

/**
 * focus_reset 宏:Win+M → Win+R → Ctrl+Space → Shift。
 *
 * 把目标机摆成"能安全接收一条英文命令"的状态:
 *   - Win+M 最小化所有窗口(桌面上没有遮挡)
 *   - Win+R 打开运行框
 *   - Ctrl+Space + Shift 把输入法强制到纯 ASCII 态(状态无关,见实现注释)
 *
 * 供远程 `act` 工具复用。**单次执行,内部零重试**。
 */
esp_err_t io_launcher_focus_reset(void);

/**
 * 注入自检并等 INJ.TXT 出现。
 *
 * INJ.TXT 出现 = HID 注入真的到达了目标机(而不是"命令发出去了")。
 * 成功返回 ESP_OK,超时返回 ESP_ERR_TIMEOUT。
 *
 * 它同时是 **IME 态探针**:命令里含 % \ > @ ( ) 等符号,中文态下必然被打成
 * 中英混排 → 跑不起来 → INJ.TXT 不出现。**不出现 = 当时 IME 在中文态**。
 */
esp_err_t io_launcher_run_selftest(void);

/**
 * 上一次注入自检的结果(true = 通过)。
 *
 * 给设备状态/LED 语义用:自检失败 = "手不灵",是现场最该被看见的一类故障。
 * 设备自己就能测出来(自检命令在目标机上有没有写出标记文件)。
 */
bool io_launcher_last_selftest_ok(void);

/**
 * 单独按一次 Shift(微软拼音的"中/英文切换",toggle)。
 *
 * 只在**知道当前是中文态**时用(探针失败之后)。它不输入任何字符,即使目标机
 * 在锁屏也只会切输入法态,不会往 PIN 输入框里塞东西。
 */
esp_err_t io_launcher_ime_toggle(void);

/**
 * 拉起 watcher(远程入口):**探针 → 启动命令**。
 *
 * 探针(INJ.TXT)先确认 IME 当时在英文态,再打启动命令;探针不过就**不自动
 * 重试**,返回 ESP_ERR_INVALID_STATE(注入是破坏性操作,重试=持续破坏),
 * 由调用方决定要不要先来一次 io_launcher_ime_toggle()。
 *
 * @param ime_toggle true = 探针之前先盲按一次 Shift 切输入法态(50/50)
 */
esp_err_t io_launcher_run_launch(bool ime_toggle);

#ifdef __cplusplus
}
#endif
