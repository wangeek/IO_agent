#!/usr/bin/env python3
"""生成 WorkBuddy 连接器图标:点阵版 SOPHON(用设备屏上那套 5x7 字模)。

为什么这么做
    用户在设备屏上看到的品牌屏是「橘色书脊 + SOPHON + 下划线 + 四色」。
    图标要让人一眼认出"就是那块屏上的东西",所以:
      · 字形**直接取设备固件里的 5x7 字模**(firmware/io_lcd/font_glcd5x7.c),
        不是另找字体 —— 屏幕上和图标上是同一套字;
      · 颜色用"四色锁死"里的两色:强调 #FF622E(文字/书脊)、面板 #F4F5F6(底);
      · 版式沿用品牌屏:左侧橘色书脊 + 右侧点阵字 + 下方短下划线。

点阵怎么摆
    ★ 为什么是**双行** SOP / HON ★
    128px 图标里塞 6 个 5x7 点阵字母 = 每行 30 个点 ⇒ 每点约 2.8px、缝 0.7px,
    栅格化时缝隙被吃掉,**点阵糊成实心字**(实测:看着像普通粗体,看不出点)。
    拆两行后每行 3 个字母 ⇒ 点约 6px、缝约 2.4px,点阵清晰可见。
    选 "SOP"/"HON" 断行,是因为两段等宽(3 个字母),视觉最稳。
    这是像素预算的硬限制,不是审美偏好。

用法
    python tools/gen_icon.py            # 出 wb-connector/icon.svg + 预览 PNG
    python tools/gen_icon.py --png      # 额外出一张 512px PNG(可选,平台也收 PNG)
"""

import argparse
import os
import re
import sys

from PIL import Image, ImageDraw

FONT_C = r"D:\Lab\IO_agent\firmware\io_lcd\font_glcd5x7.c"
OUT_SVG = r"D:\Lab\IO_agent\wb-connector\icon.svg"
OUT_PNG = r"D:\Lab\IO_agent\wb-connector\icon.png"
PREVIEW = r"D:\Lab\IO_agent\dist\icon-preview"

# 四色锁死(与 io_lcd.h 一致)
BG = "#E2E4E6"        # 哑光浅灰
PANEL = "#F4F5F6"     # 米灰白
TEXT = "#222427"      # 炭黑
ACCENT = "#FF622E"    # 亮橘橙

WORDS = ["SOP", "HON"]   # 双行:SOPHON 拆两行,点数翻倍才看得出"点阵"
GAP_RATIO = 0.40         # 点间缝隙 = dot * GAP_RATIO
LETTER_GAP_RATIO = 1.0   # 字母间距 = dot * LETTER_GAP_RATIO
LINE_GAP_RATIO = 3.0     # 行距 = dot * LINE_GAP_RATIO
MIN_DOT = 3.5            # 点小于这个就糊成实心,断言拦住

SIZE = 128
SPINE_X = 15.0        # 书脊左边
SPINE_W = 7.0         # 书脊宽度
GAP_SPINE_TEXT = 10.0  # 书脊到第一个字
PAD_R = 12.0          # 右边距


def load_glyphs():
    """从固件字库读出需要的字形(5 个列字节,bit0=最上行)。"""
    with open(FONT_C, "r", encoding="utf-8") as f:
        text = f.read()
    glyphs = {}
    for m in re.finditer(
            r"\{\s*(0x[0-9A-Fa-f]{2}),\s*(0x[0-9A-Fa-f]{2}),\s*(0x[0-9A-Fa-f]{2}),"
            r"\s*(0x[0-9A-Fa-f]{2}),\s*(0x[0-9A-Fa-f]{2})\s*\},\s*/\*\s*'(.)'\s*\*/", text):
        ch = m.group(6)
        glyphs[ch] = [int(m.group(i), 16) for i in range(1, 6)]
    missing = [c for c in set("".join(WORDS)) if c not in glyphs]
    if missing:
        raise SystemExit("font is missing glyphs: %s" % missing)
    return glyphs


def plan(glyphs=None):
    """算版式。★ 点的大小是从可用宽度**反推**的,不是写死的 ★

    踩过的坑:
      ① 把 dot 写死 ⇒ 6 字母算出 138 > 128,图溢出被裁、书脊压在字上;
      ② 只调缝隙比例不会让字变小 —— word_w 是"用满可用宽度"反解出来的,
         改比例只是在"点大缝小"和"点小缝大"之间换,总数不变。
    正确顺序:先定可用宽 → 解出 dot → 排坐标 → 断言守边界。
    """
    text_x = SPINE_X + SPINE_W + GAP_SPINE_TEXT
    avail_w = SIZE - text_x - PAD_R

    max_len = max(len(w) for w in WORDS)
    # avail_w = n*(5*cell - gap) + (n-1)*letter_gap
    per_dot = max_len * (5 * (1 + GAP_RATIO) - GAP_RATIO) \
        + (max_len - 1) * LETTER_GAP_RATIO
    dot = avail_w / per_dot

    cell = dot * (1 + GAP_RATIO)
    letter_w = 5 * cell - dot * GAP_RATIO
    letter_gap = dot * LETTER_GAP_RATIO
    line_w = max_len * letter_w + (max_len - 1) * letter_gap
    line_h = 7 * cell - dot * GAP_RATIO
    line_gap = dot * LINE_GAP_RATIO

    n = len(WORDS)
    underline_h = max(2.6, dot * 0.75)
    gap_ul = dot * 2.0
    block_h = n * line_h + (n - 1) * line_gap + gap_ul + underline_h
    y0 = (SIZE - block_h) / 2.0

    underline_y = y0 + n * line_h + (n - 1) * line_gap + gap_ul

    # 书脊夹住整个字块,上下各多出一点
    spine_top = y0 - dot * 1.4
    spine_h = (n * line_h + (n - 1) * line_gap) + dot * 2.8

    p = {
        "dot": dot, "cell": cell, "letter_w": letter_w, "letter_h": line_h,
        "letter_gap": letter_gap, "line_gap": line_gap, "line_w": line_w,
        "x0": text_x, "y0": y0,
        "spine_x": SPINE_X, "spine_w": SPINE_W,
        "spine_top": spine_top, "spine_h": spine_h,
        "underline_y": underline_y, "underline_h": underline_h,
        "underline_w": line_w * 0.45,
    }

    # ★ 边界断言:排版错了立刻报,而不是生成一张被裁的图 ★
    right = p["x0"] + p["line_w"]
    bottom = p["underline_y"] + p["underline_h"]
    if right > SIZE - 4:
        raise SystemExit("text overflows right: %.1f > %d" % (right, SIZE - 4))
    if bottom > SIZE - 4:
        raise SystemExit("underline overflows bottom: %.1f" % bottom)
    if p["spine_top"] < 8 or p["spine_top"] + p["spine_h"] > SIZE - 8:
        raise SystemExit("spine out of safe area: top=%.1f h=%.1f"
                         % (p["spine_top"], p["spine_h"]))
    if dot < MIN_DOT:
        raise SystemExit("dot=%.2f < %.1f: 点太小会糊成实心,双行需要更多空间"
                         % (dot, MIN_DOT))
    return p


def dots_for(glyphs):
    """产出 [(x, y)] 的点列表(相对 128 画布)。逐行按 WORDS 排。"""
    p = plan()
    out = []
    for li, word in enumerate(WORDS):
        ly = p["y0"] + li * (p["letter_h"] + p["line_gap"])
        for i, ch in enumerate(word):
            gx = p["x0"] + i * (p["letter_w"] + p["letter_gap"])
            cols = glyphs[ch]
            for c in range(5):
                for r in range(7):
                    if (cols[c] >> r) & 1:            # bit0 = 最上行
                        out.append((gx + c * p["cell"], ly + r * p["cell"]))
    return out, p


def write_svg(dots, p):
    rects = "\n".join(
        '  <rect x="%.2f" y="%.2f" width="%.2f" height="%.2f" rx="%.2f"/>'
        % (x, y, p["dot"], p["dot"], p["dot"] * 0.28) for x, y in dots)

    svg = f'''<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {SIZE} {SIZE}" width="{SIZE}" height="{SIZE}">
  <!-- 远控智子 Sophon — 点阵字取设备小屏同一套 5x7 字模,四色锁死 -->
  <rect width="{SIZE}" height="{SIZE}" rx="26" fill="{BG}"/>
  <rect x="6" y="6" width="{SIZE - 12}" height="{SIZE - 12}" rx="21" fill="{PANEL}"/>
  <rect x="{p['spine_x']:.1f}" y="{p['spine_top']:.2f}" width="{p['spine_w']:.1f}" height="{p['spine_h']:.2f}" rx="{p['spine_w'] / 2:.1f}" fill="{ACCENT}"/>
  <g fill="{ACCENT}">
{rects}
  </g>
  <rect x="{p['x0']:.2f}" y="{p['underline_y']:.2f}" width="{p['underline_w']:.2f}" height="{p['underline_h']:.2f}" rx="{p['underline_h'] / 2:.2f}" fill="{ACCENT}"/>
</svg>
'''
    with open(OUT_SVG, "w", encoding="utf-8", newline="\n") as f:
        f.write(svg)


def write_png(dots, p):
    """同版式的 512px PNG(平台也收 PNG;按 4x 超采样再缩小,边缘更干净)。"""
    S = 512
    k = S / SIZE
    img = Image.new("RGB", (S, S), BG)
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([6 * k, 6 * k, S - 6 * k, S - 6 * k], radius=21 * k, fill=PANEL)
    d.rounded_rectangle([p["spine_x"] * k, p["spine_top"] * k,
                         (p["spine_x"] + p["spine_w"]) * k,
                         (p["spine_top"] + p["spine_h"]) * k],
                        radius=p["spine_w"] / 2 * k, fill=ACCENT)
    for x, y in dots:
        d.rectangle([x * k, y * k, (x + p["dot"]) * k, (y + p["dot"]) * k], fill=ACCENT)
    d.rounded_rectangle([p["x0"] * k, p["underline_y"] * k,
                         (p["x0"] + p["underline_w"]) * k, (p["underline_y"] + p["underline_h"]) * k],
                        radius=p["underline_h"] / 2 * k, fill=ACCENT)
    img.save(OUT_PNG)
    return OUT_PNG


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--png", action="store_true", help="同时输出 512px PNG")
    args = ap.parse_args()

    glyphs = load_glyphs()
    dots, p = dots_for(glyphs)
    write_svg(dots, p)

    print("glyphs : %s" % ", ".join("%s=%s" % (c, glyphs[c]) for c in sorted(set("".join(WORDS)))))
    print("dots   : %d 个" % len(dots))
    print("layout : line_w=%.1f line_h=%.1f x0=%.1f y0=%.1f"
          % (p["line_w"], p["letter_h"], p["x0"], p["y0"]))
    print("wrote  : %s" % OUT_SVG)

    if args.png:
        print("wrote  : %s" % write_png(dots, p))

    # 预览:4x 放大,便于人眼检查(和真机上同一观感)
    img = Image.new("RGB", (SIZE, SIZE), BG)
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([6, 6, SIZE - 6, SIZE - 6], radius=21, fill=PANEL)
    d.rounded_rectangle([p["spine_x"], p["spine_top"], p["spine_x"] + p["spine_w"],
                         p["spine_top"] + p["spine_h"]],
                        radius=p["spine_w"] / 2, fill=ACCENT)
    for x, y in dots:
        d.rectangle([x, y, x + p["dot"], y + p["dot"]], fill=ACCENT)
    d.rounded_rectangle([p["x0"], p["underline_y"],
                         p["x0"] + p["underline_w"], p["underline_y"] + p["underline_h"]],
                        radius=p["underline_h"] / 2, fill=ACCENT)
    os.makedirs(os.path.dirname(PREVIEW), exist_ok=True)
    img.resize((SIZE * 4, SIZE * 4), Image.NEAREST).save(PREVIEW + "-4x.png")
    img.save(PREVIEW + ".png")
    print("preview: %s (+ -4x)" % (PREVIEW + ".png"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
