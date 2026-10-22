/*
 * io_hid — HID 注入层(设备的手)
 *
 * 协议:单 HID 接口 + 复合报告描述符
 *   Report ID 1 = 键盘(载荷 8 B:modifier + reserved + keycode[6])
 *   Report ID 2 = 鼠标(载荷 4 B:buttons + X + Y + Wheel)
 *
 * TinyUSB 的 tud_hid_report() 会在载荷前自动补 1 字节 Report ID,
 * 所以实际中断传输长度是 9 B / 5 B,与 io_usb.c 里的报告描述符严格一致。
 *
 * 设计约束(协议冻结):
 *   - 鼠标相对位移单次上报钳位在 ±127(HID 相对坐标是 1 字节有符号),
 *     更大的位移在这里自动切片成多次上报。
 *   - 键盘按下后必须能回到基线:io_hid_keyboard_release() 发全 0 报告。
 *   - 只注入,不读取主机键盘状态;检测到物理键盘被使用时应由上层中止
 *     (这属于 watcher / MCP 侧的安全基线,不在本模块)。
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 报告 ID(与 io_usb.c 的 HID 报告描述符一致) */
#define IO_HID_RID_KEYBOARD   1
#define IO_HID_RID_MOUSE      2

/** 修饰键位(USB HID Keyboard/Keypad page 0x07,modifier byte) */
#define IO_HID_MOD_LCTRL      0x01
#define IO_HID_MOD_LSHIFT     0x02
#define IO_HID_MOD_LALT       0x04
#define IO_HID_MOD_LGUI       0x08

/** 常用键码(USB HID Keyboard/Keypad page 0x07) */
#define IO_HID_KEY_ENTER      0x28
#define IO_HID_KEY_ESC        0x29
#define IO_HID_KEY_BACKSPACE  0x2A
#define IO_HID_KEY_TAB        0x2B
#define IO_HID_KEY_SPACE      0x2C
#define IO_HID_KEY_LEFTSHIFT  0xE1    /* 左 Shift:作为 keycode 用(IME 中英切换) */
#define IO_HID_KEY_LEFTGUI    0xE2    /* 左 Win */

/** 鼠标按键位 */
#define IO_HID_MOUSE_LEFT     0x01
#define IO_HID_MOUSE_RIGHT    0x02
#define IO_HID_MOUSE_MIDDLE   0x04

/** 初始化(目前只做状态复位;USB 栈由 io_usb 负责)。 */
esp_err_t io_hid_init(void);

/** HID 端点是否就绪(主机已枚举且上一份报告已发完)。 */
bool io_hid_ready(void);

/** 端点状态探针(诊断用):打印 HID IN 端点的 busy/claim 状态。 */
void io_hid_ep_probe(const char *tag);

/**
 * 等 HID 端点空出来(有上限)。
 *
 * 为什么需要它:**不能用 tud_hid_ready() 当"发不出去"的理由** —— 它含
 * `usbd_edpt_busy()` 项,端点正忙时返回 false,会被误判成链路故障。
 * 链路活性判据是 tud_ready()(见 io_usb.h);"能不能发"要用有上限的等待来判断。
 */
bool io_hid_wait_ready(uint32_t timeout_ms);

/** 按下一个键(可叠加,最多 6 个同时按下)。 */
esp_err_t io_hid_keyboard_press(uint8_t keycode);

/** 抬起所有已按下的键(回到基线)。 */
esp_err_t io_hid_keyboard_release(void);

/** 点按一次:按下 → 保持 hold_ms → 抬起。 */
esp_err_t io_hid_keyboard_tap(uint8_t keycode, uint16_t hold_ms);

/**
 * 组合键:按住 modifier 同时敲 keycode,保持 hold_ms 后全部抬起。
 * 例:io_hid_hotkey(IO_HID_MOD_LGUI, 'R', 50) = Win+R
 */
esp_err_t io_hid_hotkey(uint8_t modifier, uint8_t keycode, uint16_t hold_ms);

/**
 * 文本注入(US 键盘布局,ASCII 0x20..0x7E)。
 *
 * 逐字符:按下(带修饰)→ 保持 per_char_ms → 抬起。整串按**本地定时**执行,
 * 不做逐条往返。
 *
 * @param text        待注入的字符串;遇到 '\n' 自动当作 Enter
 * @param per_char_ms 每个字符的按下保持时长
 */
esp_err_t io_hid_type_text(const char *text, uint16_t per_char_ms);

/**
 * US 布局表自检(结构性不变量:大小写共用 keycode、字母连续、数字映射正确)。
 * 返回 false 表示表被改坏了 —— 宁可拒绝注入也不要打错字。
 */
bool io_hid_layout_selfcheck(void);

/**
 * 查一个字符对应的 (modifier, keycode)。支持 ASCII 0x20..0x7E。
 * 不支持的字符返回 false。
 */
bool io_hid_key_for_char(char c, uint8_t *mod, uint8_t *key);

/** 鼠标相对移动(自动按 ±127 切片)。 */
esp_err_t io_hid_mouse_move(int16_t dx, int16_t dy);

/** 鼠标滚轮(纵向,1 字节有符号)。 */
esp_err_t io_hid_mouse_scroll(int8_t dy);

/** 设置鼠标按键状态(位掩码)。 */
esp_err_t io_hid_mouse_buttons(uint8_t buttons);

#ifdef __cplusplus
}
#endif
