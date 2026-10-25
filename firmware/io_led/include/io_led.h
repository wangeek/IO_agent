/*
 * io_led — 把设备状态码真正点亮到实体 LED(WS2812,可选)。
 *
 * 状态码的**语义**是设备端早就有的(README 决策表「LED 语义」那一行):
 *   0 就绪    绿色常亮
 *   1 启动中  蓝色快闪
 *   2 网络未就绪 黄色慢闪
 *   3 动作执行中 蓝色常亮
 *   4 故障    红色常亮
 * 本组件只做最后一件事:把这个 int 变成灯。
 *
 * 为什么用 RMT 而不是 bit-bang:WS2812 的位周期是 0.4/0.85 µs
 * (800 kHz),而 `esp_cpu_delay_us` 的精度是 ±10 µs 级别,根本发不出
 * 稳定时序 ⇒ 只有 RMT(硬件按符号计时)能驱动。RMT 也不在 IDF 组件
 * 管理器里(esp_driver_rmt 是 IDF 自带组件)⇒ 不破坏「离线可复现」。
 *
 * 状态机放在这里(而不是 MCP 的 status 里算)是因为 LED 是**物理输出**:
 * 没人调 status 的时候它也得自己亮。
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 状态码(与 README 决策表一致,`status.led` 返回的就是它) */
#define IO_LED_READY       0
#define IO_LED_BOOTING     1
#define IO_LED_NET_DOWN    2
#define IO_LED_ACT_BUSY    3
#define IO_LED_FAULT       4

/**
 * 初始化 LED 通道(建 RMT + 起刷新 task)。
 *
 * 编译期开关 CONFIG_SOPHON_LED_WS2812 关掉时,这里什么都不做,
 * `io_led_state()` 仍然返回正确的状态码(只是没有灯)。
 */
esp_err_t io_led_init(void);

/** 当前状态码。 */
int io_led_state(void);

/**
 * 动作执行中(注入/下发/OTA 这类长操作)。
 *
 * 用**计数器**而不是 bool:长动作有三个来源(io_mcp 的单例槽、io_ota 的
 * OTA 任务),它们会重叠(比如 OTA 期间来了一次注入)—— bool 会被后一个
 * 覆盖掉,把前一个的"忙"抹掉。
 *
 * 依赖方向:io_mcp / io_ota → io_led。反过来(io_led → io_mcp)会成环
 * (io_mcp 要读状态码,io_led 要知道忙不忙),所以走"推"而不是"拉"。
 */
void io_led_busy_acquire(void);
void io_led_busy_release(void);

#ifdef __cplusplus
}
#endif
