/*
 * io_lcd_font — 屏上用的点阵字库(5x7,可打印 ASCII)。
 *
 * 字模数据在 font_glcd5x7.c(由 tools/extract_font5x7.py 从 Adafruit GFX 的
 * glcdfont.c 抽出来)。头文件只放尺寸和访问宏。
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 一个字形 5 列 × 7 行(第 8 行留作行距,在 io_lcd.c 里补) */
#define IO_LCD_FONT5X7_W        5
#define IO_LCD_FONT5X7_H        7
/* 字符格子:字形 5x7 + 右 1 列空隙 + 下 1 行空隙 = 6x8 */
#define IO_LCD_FONT5X7_CELL_W   6
#define IO_LCD_FONT5X7_CELL_H   8

/* 可打印 ASCII 的个数(0x20..0x7E) */
#define IO_LCD_FONT5X7_GLYPHS   95
/* 第一个字形对应的 ASCII 码 */
#define IO_LCD_FONT5X7_FIRST    0x20

extern const uint8_t io_lcd_font5x7[IO_LCD_FONT5X7_GLYPHS][IO_LCD_FONT5X7_W];
extern const uint8_t io_lcd_font5x7_count;

#ifdef __cplusplus
}
#endif
