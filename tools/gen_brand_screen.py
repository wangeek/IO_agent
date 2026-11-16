#!/usr/bin/env python3
"""生成 dongle 小屏的"品牌屏"位图 —— 中文在宿主渲染,固件只搬运。

为什么在宿主渲染
    那块屏是 160x80 且固件里**没有中文字模**(早期为省 flash 明确不做)。
    与其往固件塞 CJK 字库,不如把整屏排版在宿主画好、压成位图存进固件 ——
    固件只做"解包 + 上屏",不碰字体,符合哑设备原则(只搬运不解读)。

为什么是 2bpp 索引色
    主题是"四色锁死"(底/面板/正文/强调),正好 2 位一个像素:
      160x80 / 4 = 3200 字节(比 RGB565 的 25600 字节省 8 倍)。
    四色之外不谈抗锯齿 —— 这是刻意取舍:0.96 寸屏上糊边比硬边更难看,
    而且"四色锁死"本来就是设计约束。

排版照搬 main.c 的现有参数(改这里就要同步改那边):
    底 #E2E4E6 铺满;面板 #F4F5F6 铺 0..64;左边 4px 橘色书脊;
    SOPHON 大字 (12,8) 橘色;下划线 (12,28) 74x2 橘色;
    IP 行 (12,38)、状态行 (12,52) —— **这两行留给固件画**,位图里留空面板色,
    因为 IP 和状态是运行期才知道的。
    底部 64..80 的 16px 带宽放作者署名。

用法:
    python tools/gen_brand_screen.py                 # 出位图 + 预览图
    python tools/gen_brand_screen.py --preview-only  # 只看效果,不写 C 文件
"""

import argparse
import os
import sys

from PIL import Image, ImageDraw, ImageFont

W, H = 160, 80

# 四色锁死(与 io_lcd.h 的 IO_LCD_THEME_* 一致)
BG = (0xE2, 0xE4, 0xE6)      # 哑光浅灰
PANEL = (0xF4, 0xF5, 0xF6)   # 米灰白
TEXT = (0x22, 0x24, 0x27)    # 炭黑
ACCENT = (0xFF, 0x62, 0x2E)  # 亮橘橙

PALETTE = [BG, PANEL, TEXT, ACCENT]
IDX_BG, IDX_PANEL, IDX_TEXT, IDX_ACCENT = 0, 1, 2, 3

# 排版(与 firmware/main/main.c 同步)
PANEL_H = 62        # 面板高度:给底部作者带留 18px(15px 汉字才不贴边)
SPINE_W = 4
TITLE_XY = (12, 3)
UNDERLINE = (12, 27, 0, 2)      # x, y, 宽度(0 表示=标题实际宽度), 高
IP_Y, STATE_Y = 36, 50          # 留给固件画,位图里保持面板色
AUTHOR_BAND_Y = PANEL_H         # 底部属名带

AUTHOR = "网瘾老登干点啥"

CN_FONT_CANDIDATES = [
    r"C:\Windows\Fonts\msyhbd.ttc",   # 微软雅黑 Bold
    r"C:\Windows\Fonts\msyh.ttc",
    r"C:\Windows\Fonts\simhei.ttf",
    r"C:\Windows\Fonts\Dengb.ttf",
]
LATIN_FONT_CANDIDATES = [
    r"C:\Windows\Fonts\msyhbd.ttc",
    r"C:\Windows\Fonts\arialbd.ttf",
    r"C:\Windows\Fonts\seguisb.ttf",
]

DST_C = r"D:\Lab\IO_agent\firmware\io_lcd\brand_screen.c"
DST_H = r"D:\Lab\IO_agent\firmware\io_lcd\include\brand_screen.h"
DST_PNG = r"D:\Lab\IO_agent\firmware\io_lcd\brand_preview.png"


def pick(cands):
    for p in cands:
        if os.path.exists(p):
            return p
    raise SystemExit("no usable font among %s" % cands)


def nearest_index(rgb):
    best, best_d = 0, 1 << 30
    for i, c in enumerate(PALETTE):
        d = (rgb[0] - c[0]) ** 2 + (rgb[1] - c[1]) ** 2 + (rgb[2] - c[2]) ** 2
        if d < best_d:
            best, best_d = i, d
    return best


def stamp_text(img, xy, text, font, color, anchor="la", thresh=110):
    """
    把文字以**纯色**贴上:先渲成灰度遮罩 → 阈值二值化 → 用该色填充。

    为什么不直接 draw.text(带抗锯齿)再整体量化:
      灰度中间值到橘色 #FF622E 的距离可能**比到炭黑更近**
      (实测 (150,150,150) 到橘 24545、到炭黑 38773)⇒ 汉字边缘会被量化成
      **橘色噪点**。按层二值化就不存在中间值,从根上不会错色;
      而 2bpp 四色本来就容不下抗锯齿。
    """
    mask = Image.new("L", img.size, 0)
    ImageDraw.Draw(mask).text(xy, text, font=font, fill=255, anchor=anchor)
    mask = mask.point(lambda v: 255 if v >= thresh else 0)
    img.paste(color, (0, 0), mask)


def stamp_ellipse(img, cx, cy, r, color):
    mask = Image.new("L", img.size, 0)
    ImageDraw.Draw(mask).ellipse([cx - r, cy - r, cx + r, cy + r], fill=255)
    mask = mask.point(lambda v: 255 if v >= 128 else 0)
    img.paste(color, (0, 0), mask)


def build_image():
    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)

    # 面板 + 书脊(纯色块,不需要抗锯齿)
    d.rectangle([0, 0, W - 1, PANEL_H - 1], fill=PANEL)
    d.rectangle([0, 0, SPINE_W - 1, PANEL_H - 1], fill=ACCENT)

    # ---- 标题:SOPHON
    # 字号按"必须给下面两行留位置"倒推:标题占 4..26,下划线 28..30,
    # IP 38..46,状态 52..60(行高 8 = 5x7 字模 + 1 行距)。
    title_font = ImageFont.truetype(pick(LATIN_FONT_CANDIDATES), 20)
    tx, ty = TITLE_XY
    stamp_text(img, (tx, ty), "SOPHON", title_font, ACCENT, anchor="la")

    tw = d.textlength("SOPHON", font=title_font)
    if tx + tw > W - 8:
        raise SystemExit("title overflows: %.1f px at x=%d" % (tw, tx))

    # 下划线:宽度跟着标题实际宽度走(别写死)
    ux, uy, _uw, uh = UNDERLINE
    d.rectangle([ux, uy, int(ux + tw) - 1, uy + uh - 1], fill=ACCENT)

    # ---- IP / 状态两行留空 —— 固件画(运行期才知道)
    for y0 in (IP_Y, STATE_Y):
        if y0 < uy + uh:
            raise SystemExit("IP/state row %d collides with title block" % y0)

    # ---- 底部作者带:右对齐 + 一枚橘色小点
    # 15px 是实测下限:13/14px 二值化后笔画互相吃掉,认不出字;16px 会顶满 16px 带。
    author_font = ImageFont.truetype(pick(CN_FONT_CANDIDATES), 15)
    band_cy = AUTHOR_BAND_Y + (H - AUTHOR_BAND_Y) // 2
    right = W - 5

    aw = d.textlength(AUTHOR, font=author_font)
    if right - aw - 9 < SPINE_W + 4:
        raise SystemExit("author line overflows: %.1f px" % aw)
    stamp_text(img, (right, band_cy), AUTHOR, author_font, TEXT, anchor="rm")

    dot_cx = int(right - aw) - 5
    stamp_ellipse(img, dot_cx, band_cy, 2, ACCENT)

    return img


def quantize(img):
    """→ 2bpp 索引图(每字节 4 个像素,高位在前)。"""
    px = img.load()
    out = bytearray()
    for y in range(H):
        for x in range(0, W, 4):
            byte = 0
            for k in range(4):
                byte = (byte << 2) | nearest_index(px[x + k, y])
            out.append(byte)
    return bytes(out)


def write_c(data, path_c, path_h):
    header = '''/*
 * brand_screen — dongle 小屏的"品牌屏"位图(由 tools/gen_brand_screen.py 生成)。
 *
 * 中文在这里是**像素**,不是字模 —— 固件里没有中文字库(早期为省 flash 明确不做)。
 * 排版在宿主画好、压成 2bpp 索引色,固件只做"解包 + 上屏",不碰字体
 * (哑设备原则:只搬运,不解读)。
 *
 * 布局(与 firmware/main/main.c 同步):
 *   底 #E2E4E6 / 面板 #F4F5F6 铺 0..%d / 左 4px 橘色书脊
 *   SOPHON 大字 + 下划线 = 位图里画好
 *   **IP 行(y=%d)与状态行(y=%d)留空** —— 那是运行期才知道的,固件画
 *   底部 y=%d..%d 放作者署名
 *
 * 尺寸:160x80,2 位一像素 = %d 字节。
 *
 * ★ 不要手改这个文件 ★ 改 tools/gen_brand_screen.py 重新生成。
 */
#pragma once

#include <stdint.h>

#include "io_lcd.h"

#ifdef __cplusplus
extern "C" {
#endif

#define IO_BRAND_W       %d
#define IO_BRAND_H       %d
#define IO_BRAND_BYTES   %d

/* 运行期文字的位置(固件画 IP / 状态时用这两个) —— 与位图留空区对应 */
#define IO_BRAND_TEXT_X  %d
#define IO_BRAND_IP_Y    %d
#define IO_BRAND_STATE_Y %d

/* 调色板:下标即像素值(0=底 1=面板 2=正文 3=强调) */
extern const uint16_t io_brand_palette[4];

/* 2bpp 索引位图,行优先,每字节 4 像素(高位在前) */
extern const uint8_t io_brand_pixels[IO_BRAND_BYTES];

#ifdef __cplusplus
}
#endif
''' % (PANEL_H, IP_Y, STATE_Y, AUTHOR_BAND_Y, H, len(data),
       W, H, len(data), TITLE_XY[0], IP_Y, STATE_Y)

    body = '''/*
 * brand_screen — 由 tools/gen_brand_screen.py 生成,不要手改。
 */
#include "brand_screen.h"

/* 四色锁死:底 / 面板 / 正文 / 强调 */
const uint16_t io_brand_palette[4] = {
    IO_LCD_THEME_BG,
    IO_LCD_THEME_PANEL,
    IO_LCD_THEME_TEXT,
    IO_LCD_THEME_ACCENT,
};

const uint8_t io_brand_pixels[IO_BRAND_BYTES] = {
'''
    lines = []
    for i in range(0, len(data), 16):
        chunk = data[i:i + 16]
        lines.append("    " + ", ".join("0x%02X" % b for b in chunk) + ",")
    body += "\n".join(lines) + "\n};\n"

    with open(path_h, "w", encoding="utf-8", newline="\n") as f:
        f.write(header)
    with open(path_c, "w", encoding="utf-8", newline="\n") as f:
        f.write(body)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--preview-only", action="store_true")
    args = ap.parse_args()

    img = build_image()

    data = quantize(img)

    # 关键:把**量化后**的图也存出来。
    # 设计稿有灰度抗锯齿,但设备是 2bpp 四色 —— 真正上屏的是二值化后的样子。
    # 只审美化稿会看错(汉字笔画在量化后会变粗、甚至糊掉内腔)。
    q = Image.new("RGB", (W, H))
    qp = q.load()
    for i, byte in enumerate(data):
        base = i * 4
        y, x0 = divmod(base, W)
        for k in range(4):
            qp[x0 + k, y] = PALETTE[(byte >> (6 - 2 * k)) & 0x03]

    img.resize((W * 4, H * 4), Image.NEAREST).save(
        DST_PNG.replace(".png", "_4x.png"))
    q.resize((W * 4, H * 4), Image.NEAREST).save(
        DST_PNG.replace(".png", "_quant_4x.png"))
    img.save(DST_PNG)
    print("preview: %s (+ _4x = 设计稿, _quant_4x = 上屏效果)" % DST_PNG)
    print("bitmap: %d bytes (2bpp, %dx%d)" % (len(data), W, H))

    diff = sum(1 for y in range(H) for x in range(W)
               if q.getpixel((x, y)) != img.getpixel((x, y)))
    print("roundtrip: %d/%d pixels differ (%.2f%%)"
          % (diff, W * H, 100.0 * diff / (W * H)))

    if args.preview_only:
        return 0

    write_c(data, DST_C, DST_H)
    print("wrote %s" % DST_C)
    print("wrote %s" % DST_H)
    return 0


if __name__ == "__main__":
    sys.exit(main())
