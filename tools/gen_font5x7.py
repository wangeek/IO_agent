"""
生成 5x7 点阵字库(firmware/io_lcd/font_glcd5x7.c)+ 预览 PNG。

为什么自己生成,不用现成的:
  厂商 demo 里的 TFT_eSPI/Fonts/glcdfont.c 是"Original Adafruit_GFX 5x7 font",
  但它的**字符顺序不是线性的**(实测:字形 33 是 '!'、字形 35 是 'H'),
  按 ASCII 顺序索引会取到完全错的字形 ⇒ 不用。
  自己光栅化 ⇒ 顺序天然正确,而且能先把预览图渲染出来**用眼睛验**。

做法:
  1. 用 PIL 把字符以**高倍字号**(默认 48 px)画出来 —— 小字号直接光栅化会塌
     (实测 Lucida Console 9 px 的 'A' 塌成一条斜线,完全不可读);
  2. 按 cap height 换算出"每个输出行对应多少高分辨率像素",把墨迹摆进
     5x7 目标画布(横向在 5 宽里居中,纵向按**基线**对齐:大写顶到第 0 行,
     小写坐在同一基线,降部切掉);
  3. BOX 降采样到 5x7,阈值二值化;bit0 = 最上面一行(和 Adafruit GFX 一致)。

用法:
  python tools\gen_font5x7.py              # 生成字库 + 预览图
  python tools\gen_font5x7.py --font consola
"""
import os
import sys

from PIL import Image, ImageDraw, ImageFont

DST_C = r"D:\Lab\IO_agent\firmware\io_lcd\font_glcd5x7.c"
DST_PNG = r"D:\Lab\IO_agent\firmware\io_lcd\font_preview.png"
DST_MOCK = r"D:\Lab\IO_agent\firmware\io_lcd\font_preview_screen.png"

FONT_CANDIDATES = {
    "consola": r"C:\Windows\Fonts\consola.ttf",
    "lucon":   r"C:\Windows\Fonts\lucon.ttf",
    "cour":    r"C:\Windows\Fonts\cour.ttf",
    "verdana": r"C:\Windows\Fonts\verdana.ttf",
    "tahoma":  r"C:\Windows\Fonts\tahoma.ttf",
    "arial":   r"C:\Windows\Fonts\arial.ttf",
}
DEFAULT_FONT = "consola"
BIG_SIZE = 48          # 高倍渲染字号
THRESHOLD = 110

GLYPH_W = 5
GLYPH_H = 7
CELL_W = 6
CELL_H = 8
FIRST = 0x20
LAST = 0x7E

OUT_HEADER = '''/*
 * font_glcd5x7 — 5x7 点阵字库(可打印 ASCII 0x20..0x7E)。
 *
 * 出处:本文件由 tools/gen_font5x7.py 生成 —— 把 Windows 的 %s
 *      以 %d px 高倍光栅化,BOX 降采样到 5x7 后二值化。
 *
 * 为什么不直接用厂商 demo 里的 TFT_eSPI/Fonts/glcdfont.c:
 *      那份确实写着 "Original Adafruit_GFX 5x7 font",但它的**字符顺序
 *      不是线性的**(实测字形 33 是 '!'、字形 35 是 'H'),按 ASCII 索引
 *      会取到完全错的字形 ⇒ 自己生成,顺序天然正确。
 *
 * 编码:每个字形 5 个字节 = 5 列,**每字节的 bit0 是最上面那一行**
 *      (列优先,和 Adafruit GFX 一致)。
 *
 * 为什么只留 ASCII:0.96 寸屏 160x80,放 16 个中文字就要几十 KB flash
 *      —— 屏上不做中文(用户 2026-10-25 明确)。
 */
#include "io_lcd_font.h"

const uint8_t io_lcd_font5x7[IO_LCD_FONT5X7_GLYPHS][IO_LCD_FONT5X7_W] = {
'''

OUT_FOOTER = '''};

const uint8_t io_lcd_font5x7_count = IO_LCD_FONT5X7_GLYPHS;
'''


def pick_font(name):
    path = FONT_CANDIDATES.get(name)
    if path and os.path.exists(path):
        return path
    for p in FONT_CANDIDATES.values():
        if os.path.exists(p):
            return p
    raise SystemExit("no usable TTF found")


def draw_ink(font, ch, origin_x=16, baseline_y=64):
    """把字符高倍画一遍,返回 (墨迹图, 墨迹框, 基线 y)。"""
    canvas = Image.new("L", (BIG_SIZE * 3, BIG_SIZE * 3), 0)
    d = ImageDraw.Draw(canvas)
    d.text((origin_x, baseline_y), ch, font=font, fill=255, anchor="ls")
    bbox = canvas.getbbox()
    if bbox is None:
        return None, None, baseline_y
    return canvas.crop(bbox), bbox, baseline_y


def measure_cap(font):
    ink, bbox, baseline_y = draw_ink(font, "H")
    return baseline_y - bbox[1]


def render_char(font, ch, scale):
    """返回 5 个列字节(bit0 在最上)。scale = 每个输出行对应的高分辨率像素数。"""
    ink, bbox, baseline_y = draw_ink(font, ch)
    if ink is None:
        return [0] * GLYPH_W

    ink_w, ink_h = ink.size
    target = Image.new("L", (GLYPH_W * scale, GLYPH_H * scale), 0)

    # 横向:在 5 宽(高分辨率下 5*scale)里居中
    left = ((GLYPH_W * scale) - ink_w) // 2
    # 纵向:墨迹的**基线**对齐到画布底边(输出第 GLYPH_H 行之下)
    top = (GLYPH_H * scale) - (baseline_y - bbox[1])
    target.paste(ink, (left, top))       # 降部超出画布的部分自动裁掉

    small = target.resize((GLYPH_W, GLYPH_H), Image.BOX)
    pixels = small.load()
    cols = []
    for x in range(GLYPH_W):
        byte = 0
        for y in range(GLYPH_H):
            if pixels[x, y] >= THRESHOLD:
                byte |= 1 << y            # bit0 = 最上面一行
        cols.append(byte)
    return cols


def draw_glyph(draw, g, gx, gy, scale=1):
    for cx in range(GLYPH_W):
        for cy in range(GLYPH_H):
            if (g[cx] >> cy) & 1:
                draw.rectangle([gx + cx * scale, gy + cy * scale,
                                gx + cx * scale + scale - 1,
                                gy + cy * scale + scale - 1], fill=255)


def build_preview(glyphs, path):
    chars = [chr(c) for c in range(FIRST, LAST + 1)]
    cols = 16
    rows = (len(chars) + cols - 1) // cols
    pad = 1
    img = Image.new("L", (cols * (CELL_W + pad) + pad,
                          rows * (CELL_H + pad) + pad), 0)
    d = ImageDraw.Draw(img)
    for i, ch in enumerate(chars):
        gx = (i % cols) * (CELL_W + pad) + pad
        gy = (i // cols) * (CELL_H + pad) + pad
        draw_glyph(d, glyphs[i], gx, gy)
    img.save(path)
    return img.size


def build_screen_mock(glyphs, path, text_lines):
    """模拟 160x80 的屏:确认排版在真实尺寸下读得清。"""
    img = Image.new("L", (160, 80), 0)
    d = ImageDraw.Draw(img)
    for line_no, line in enumerate(text_lines):
        for i, ch in enumerate(line):
            code = ord(ch)
            if code < FIRST or code > LAST:
                continue
            gx = 2 + i * CELL_W
            gy = 2 + line_no * CELL_H
            draw_glyph(d, glyphs[code - FIRST], gx, gy)
    img.save(path)
    return img.size


def main():
    name = DEFAULT_FONT
    if "--font" in sys.argv:
        name = sys.argv[sys.argv.index("--font") + 1]
    font_path = pick_font(name)
    font = ImageFont.truetype(font_path, BIG_SIZE)

    cap = measure_cap(font)
    scale = max(1, cap // GLYPH_H)
    print("font: %s size=%d cap=%d scale=%d"
          % (os.path.basename(font_path), BIG_SIZE, cap, scale))

    glyphs = []
    for code in range(FIRST, LAST + 1):
        glyphs.append(render_char(font, chr(code), scale))

    print("preview: %s %s" % (DST_PNG, build_preview(glyphs, DST_PNG)))
    print("mock:    %s %s" % (DST_MOCK, build_screen_mock(
        glyphs, DST_MOCK,
        ["SOPHON READY",
         "NET OK 192.168",
         "BUSY: TYPING",
         "ERR 3: NO TARGET"])))

    if "--preview" in sys.argv:
        return 0

    with open(DST_C, "w", encoding="utf-8", newline="\n") as f:
        f.write(OUT_HEADER % (os.path.basename(font_path), BIG_SIZE))
        for i, code in enumerate(range(FIRST, LAST + 1)):
            g = glyphs[i]
            f.write("    { 0x%02X, 0x%02X, 0x%02X, 0x%02X, 0x%02X },  /* '%s' */\n"
                    % (g[0], g[1], g[2], g[3], g[4], chr(code)))
        f.write(OUT_FOOTER)
    print("wrote %s (%d glyphs)" % (DST_C, len(glyphs)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
