#!/usr/bin/env python3
"""渲染 logo 的三个候选版式,并排输出一张对比图供人眼挑选。

背景:128px 图标里放 6 个 5x7 点阵字母,每个点只有约 2.8px、缝隙 0.7px,
      栅格化时缝隙会被吃掉 ⇒ 点阵糊成实心字,**看不出是点阵**。
      这是像素预算的硬限制,不是参数没调好。

三个候选:
  A 单行 SOPHON(忠实于设备屏版式,但点阵感弱)
  B 双行 SOP/HON(点数翻倍 ⇒ 点阵清晰,版式仍是"书脊+字+下划线")
  C 单字 S 大点阵 + 下划线(点最大,小尺寸最醒;丢了词形)

用法: python tools/gen_icon_variants.py
"""

import os
import re
import sys

from PIL import Image, ImageDraw, ImageFont

FONT_C = r"D:\Lab\IO_agent\firmware\io_lcd\font_glcd5x7.c"
OUT = r"D:\Lab\IO_agent\dist\icon-variants"

BG = (0xE2, 0xE4, 0xE6)
PANEL = (0xF4, 0xF5, 0xF6)
ACCENT = (0xFF, 0x62, 0x2E)

S = 128
SPINE_X, SPINE_W = 15, 7
PAD_R = 12


def glyphs():
    text = open(FONT_C, encoding="utf-8").read()
    out = {}
    for m in re.finditer(
            r"\{\s*(0x[0-9A-Fa-f]{2}),\s*(0x[0-9A-Fa-f]{2}),\s*(0x[0-9A-Fa-f]{2}),"
            r"\s*(0x[0-9A-Fa-f]{2}),\s*(0x[0-9A-Fa-f]{2})\s*\},\s*/\*\s*'(.)'\s*\*/", text):
        out[m.group(6)] = [int(m.group(i), 16) for i in range(1, 6)]
    return out


def shell():
    """底板:四周留 6px 的浅灰,内层米灰白面板。"""
    img = Image.new("RGB", (S, S), BG)
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([6, 6, S - 6, S - 6], radius=21, fill=PANEL)
    return img, d


def draw_word(img, d, word, x, y, dot, gap_ratio, letter_gap_ratio):
    cell = dot * (1 + gap_ratio)
    gap = dot * gap_ratio
    lg = dot * letter_gap_ratio
    g = GLYPHS
    for i, ch in enumerate(word):
        gx = x + i * (5 * cell - gap + lg)
        cols = g[ch]
        for c in range(5):
            for r in range(7):
                if (cols[c] >> r) & 1:
                    px = gx + c * cell
                    py = y + r * cell
                    d.rectangle([px, py, px + dot, py + dot], fill=ACCENT)


def variant_a():
    """单行 SOPHON —— 与设备屏版式一致。"""
    img, d = shell()
    text_x = SPINE_X + SPINE_W + 10
    avail = S - text_x - PAD_R
    per_dot = 6 * (5 * 1.34 - 0.34) + 5 * 1.0
    dot = avail / per_dot
    word_w = avail
    letter_h = 7 * dot * 1.34 - dot * 0.34
    block = letter_h + dot * 2.2 + max(2.6, dot * 0.9)
    y0 = (S - block) / 2
    d.rounded_rectangle([SPINE_X, y0 - dot * 2.5, SPINE_X + SPINE_W,
                         y0 + letter_h + dot * 4.5], radius=SPINE_W / 2, fill=ACCENT)
    draw_word(img, d, "SOPHON", text_x, y0, dot, 0.34, 1.0)
    ul_y = y0 + letter_h + dot * 2.2
    d.rounded_rectangle([text_x, ul_y, text_x + word_w * 0.45, ul_y + 3],
                        radius=1.5, fill=ACCENT)
    return img, "A 单行 SOPHON"


def variant_b():
    """双行 SOP / HON —— 点数翻倍,点阵清晰。"""
    img, d = shell()
    text_x = SPINE_X + SPINE_W + 8
    avail = S - text_x - PAD_R
    per_dot = 3 * (5 * 1.34 - 0.34) + 2 * 1.0
    dot = avail / per_dot
    line_h = 7 * dot * 1.34 - dot * 0.34
    gap_lines = dot * 3.0
    block = line_h * 2 + gap_lines + dot * 2.2 + 3
    y0 = (S - block) / 2
    d.rounded_rectangle([SPINE_X, y0 - dot * 1.6, SPINE_X + SPINE_W,
                         y0 + line_h * 2 + gap_lines + dot * 3.2],
                        radius=SPINE_W / 2, fill=ACCENT)
    draw_word(img, d, "SOP", text_x, y0, dot, 0.34, 1.0)
    draw_word(img, d, "HON", text_x, y0 + line_h + gap_lines, dot, 0.34, 1.0)
    ul_y = y0 + line_h * 2 + gap_lines + dot * 2.2
    d.rounded_rectangle([text_x, ul_y, text_x + avail * 0.45, ul_y + 3],
                        radius=1.5, fill=ACCENT)
    return img, "B 双行 SOP/HON"


def variant_c():
    """单字 S —— 点最大,小尺寸最醒目;配下划线。"""
    img, d = shell()
    dot = 9.0
    letter_w = 5 * dot * 1.34 - dot * 0.34
    letter_h = 7 * dot * 1.34 - dot * 0.34
    block = letter_h + dot * 0.7 + 4
    x0 = SPINE_X + SPINE_W + (S - SPINE_X - SPINE_W - letter_w) / 2 - 6
    y0 = (S - block) / 2
    d.rounded_rectangle([SPINE_X, y0 - dot * 0.5, SPINE_X + SPINE_W,
                         y0 + letter_h + dot * 1.2], radius=SPINE_W / 2, fill=ACCENT)
    draw_word(img, d, "S", x0, y0, dot, 0.34, 1.0)
    ul_y = y0 + letter_h + dot * 0.7
    d.rounded_rectangle([x0, ul_y, x0 + letter_w, ul_y + 4], radius=2, fill=ACCENT)
    return img, "C 单字 S"


def main():
    global GLYPHS
    GLYPHS = glyphs()

    items = [variant_a(), variant_b(), variant_c()]
    pad = 8
    sheet = Image.new("RGB", (S * len(items) + pad * (len(items) + 1), S + 34), BG)
    x = pad
    for img, label in items:
        sheet.paste(img, (x, pad))
        d = ImageDraw.Draw(sheet)
        f = ImageFont.truetype(r"C:\Windows\Fonts\msyh.ttc", 13)
        d.text((x, S + pad + 8), label, font=f, fill=(0x22, 0x24, 0x27))
        x += S + pad

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    sheet.save(OUT + ".png")
    sheet.resize((sheet.width * 2, sheet.height * 2), Image.NEAREST).save(OUT + "-2x.png")
    print("wrote %s (.png / -2x.png)" % OUT)
    for _, label in items:
        print("  -", label)
    return 0


if __name__ == "__main__":
    sys.exit(main())
