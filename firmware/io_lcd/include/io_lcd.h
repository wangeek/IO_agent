/*
 * io_lcd — 驱动 dongle 上那块 0.96 寸 ST7735S 屏(SPI,可选)。
 *
 * ★ 硬件事实全部来自厂商资料,不是猜的 ★
 *   原理图: ESP32-S3-0.96LCD(厂商随 5 套固件一起给的那份 PDF)
 *   demo:   Arduino/lib/TFT_eSPI + examples/250428/GIF/Animation
 *
 *   面板   ST7735S,GREENTAB160x80 ⇒ 竖屏 80x160。
 *          控制器 GRAM 是 132x162,可见区在 GRAM 里**偏了**:
 *          竖屏 colstart=26/rowstart=1,横屏(旋转 1/3) colstart=1/rowstart=26。
 *   接口   SPI,**MODE0**,只写(不接 MISO,TFT_MISO=-1)。
 *   引脚   SCLK=GPIO10  MOSI=GPIO11  CS=GPIO12  DC=GPIO13  RST=GPIO14
 *          (原理图 C5「0.96 LCD SPI0 CE1」:pin3=DIN→GPIO11、pin4=CLK→GPIO10、
 *           pin5=DC→GPIO13、pin6=RST→GPIO14、pin7=CS→GPIO12)
 *   供电   VCC=3V3。
 *   背光   **没有控制脚**。LEDK(pin11)经 R2(10Ω)直接接地,阳极在模块 VCC
 *          ⇒ 背光**常亮**、亮度固定、既不能调也不能关。
 *          这也解释了厂商 Arduino demo 为什么把 `TFT_BL` 整行注释掉
 *          —— 板上就没有可写的 BL 脚。
 *   颜色   面板是 BGR 序 ⇒ MADCTL 的 BGR 位(0x08)置 1,
 *          像素按 **RGB565 原样**发(软件不换 R/B),每个像素**高字节在前**。
 *
 * 架构:画布在 PSRAM,面板是"只写终端" ⇒ 典型用法是
 *     往画布上画 → io_lcd_flush() 推一次。
 * 这样上层(JPEG 缩略图、文字)用普通 2D 画布 API 就行,
 * 不用每画一个像素就发一次 SPI。
 *
 * 编译期开关 CONFIG_SOPHON_LCD_ST7735 关掉时,这里是空实现:
 * 不建 SPI 总线、不占 GPIO、零内存零 task 零 flash 开销
 * (与 io_led 同一套路:硬件可选,但语义照算)。
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 面板原生几何(竖屏)。旋转 1/3 时宽高互换:160x80。 */
#define IO_LCD_PANEL_W_PORTRAIT   80
#define IO_LCD_PANEL_H_PORTRAIT   160

/* 16 位色(RGB565)。红绿蓝各 5/6/5 位。 */
#define IO_LCD_RGB565(r, g, b) \
    ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

/* 常用色 */
#define IO_LCD_BLACK   0x0000
#define IO_LCD_WHITE   0xFFFF
#define IO_LCD_RED     0xF800
#define IO_LCD_GREEN   0x07E0
#define IO_LCD_BLUE    0x001F

/**
 * 初始化屏硬件(SPI 总线 + 复位 + ST7735S 初始化序列 + 设定旋转 + 分配画布)。
 *
 * 阻塞约 700 ms(SWRESET 后 150 ms、SLPOUT 后 500 ms,按数据手册给足)。
 * 编译期开关关掉时直接返回 ESP_ERR_NOT_SUPPORTED。
 */
esp_err_t io_lcd_init(void);

/**
 * 设旋转。0/2 = 竖屏 80x160,1/3 = 横屏 160x80。
 * 会重设 MADCTL 与 GRAM 偏移,并把画布清成黑色(不自动刷屏)。
 */
esp_err_t io_lcd_set_rotation(uint8_t rotation);

/** 当前可见区宽/高(像素)。未初始化时返回 0。 */
int io_lcd_width(void);
int io_lcd_height(void);

/** 屏是否真的可用(硬件在位且初始化成功)。 */
bool io_lcd_available(void);

/**
 * 画布(帧缓冲):宽*高 个 uint16,原生 RGB565(红在高位),行优先。
 * 上层直接往里画,画完调 io_lcd_flush() 推到屏上。
 * 缓冲在 PSRAM 里;未初始化时返回 NULL。
 */
uint16_t *io_lcd_fb(void);

/**
 * 把画布推到屏上。逐行发送(每行一次 SPI 事务),
 * 行内字节序在这里换(高字节在前),调用者的画布不动。
 */
esp_err_t io_lcd_flush(void);

/**
 * 在画布上刷一个矩形(单色)。坐标会被裁到可见区内;越界部分被切掉,不是报错。
 * 只改画布,要看到效果还得 io_lcd_flush()。
 */
esp_err_t io_lcd_fill_rect(int x, int y, int w, int h, uint16_t rgb565);

/**
 * 在画布上画一行 ASCII(6x8 格子,5x7 字模)。
 * 超出右边界的部分被截断。只改画布。
 *
 * @return 写进去的列数(截断后)
 */
int io_lcd_text(int x, int y, const char *s, uint16_t fg, uint16_t bg);

/**
 * 清屏 + 画一段文字(自动按屏宽折行,'\n' 强制换行),然后**刷到屏上**。
 * 这是"显示一段东西"最常用的一步。
 *
 * 0.96 寸屏 160x80 ⇒ 26 列 × 10 行(ASCII,不做中文)。
 *
 * @return 实际画了几行
 */
int io_lcd_print(const char *s, uint16_t fg, uint16_t bg);

/** 整屏刷黑(背光关不掉,只能让画面变黑)。清画布 + 推到屏上。 */
esp_err_t io_lcd_blank(void);

/**
 * 自检:刷一组竖条彩条 + 一圈白边,然后推到屏上。
 *
 * 这是**唯一**能一眼分清三件事的图案:
 *   ① 红蓝有没有反(BGR 位错 ⇒ 红条变蓝条)
 *   ② GRAM 偏移对不对(偏移错 ⇒ 画面整体平移,一侧露出黑边)
 *   ③ 旋转对不对(横竖反 ⇒ 彩条变横条)
 */
esp_err_t io_lcd_selftest(void);

#ifdef __cplusplus
}
#endif
