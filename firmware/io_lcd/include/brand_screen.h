/*
 * brand_screen — dongle 小屏的"品牌屏"位图(由 tools/gen_brand_screen.py 生成)。
 *
 * 中文在这里是**像素**,不是字模 —— 固件里没有中文字库(早期为省 flash 明确不做)。
 * 排版在宿主画好、压成 2bpp 索引色,固件只做"解包 + 上屏",不碰字体
 * (哑设备原则:只搬运,不解读)。
 *
 * 布局(与 firmware/main/main.c 同步):
 *   底 #E2E4E6 / 面板 #F4F5F6 铺 0..62 / 左 4px 橘色书脊
 *   SOPHON 大字 + 下划线 = 位图里画好
 *   **IP 行(y=36)与状态行(y=50)留空** —— 那是运行期才知道的,固件画
 *   底部 y=62..80 放作者署名
 *
 * 尺寸:160x80,2 位一像素 = 3200 字节。
 *
 * ★ 不要手改这个文件 ★ 改 tools/gen_brand_screen.py 重新生成。
 */
#pragma once

#include <stdint.h>

#include "io_lcd.h"

#ifdef __cplusplus
extern "C" {
#endif

#define IO_BRAND_W       160
#define IO_BRAND_H       80
#define IO_BRAND_BYTES   3200

/* 运行期文字的位置(固件画 IP / 状态时用这两个) —— 与位图留空区对应 */
#define IO_BRAND_TEXT_X  12
#define IO_BRAND_IP_Y    36
#define IO_BRAND_STATE_Y 50

/* 调色板:下标即像素值(0=底 1=面板 2=正文 3=强调) */
extern const uint16_t io_brand_palette[4];

/* 2bpp 索引位图,行优先,每字节 4 像素(高位在前) */
extern const uint8_t io_brand_pixels[IO_BRAND_BYTES];

#ifdef __cplusplus
}
#endif
