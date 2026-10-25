/*
 * io_screen — 屏上放什么(内容层):盯共享盘上的缩略图,变了就画到屏上。
 * 详见 io_screen.c 顶部的说明(为什么是 BMP 而不是 JPEG)。
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 起内容层:开一个 task 盯共享盘上的 `SCRN.BMP`。
 *
 * 屏本身(io_lcd)由别的 task 初始化;本组件会等它起来再开始工作。
 * 屏没开(CONFIG_SOPHON_LCD_ST7735=n)时,本组件什么也不做。
 */
esp_err_t io_screen_start(void);

#ifdef __cplusplus
}
#endif
