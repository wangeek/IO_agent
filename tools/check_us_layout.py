#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
check_us_layout.py -- 核对固件的 US 键盘布局表。

为什么需要它:固件里的 s_us_map 是 95 项的手写展开表,一次写错就会**静默
打出错字**。实测踩过:US 数字行是 "1 2 3 4 5 6 7 8 9 0" 对应 "! @ # $ % ^ & * ( )",
而 & ( ) 三个的 keycode 曾经写成相邻键 —— 启动命令里的 for %d in ( ... )
括号打错,cmd 语法错,watcher 起不来,设备日志只显示"等不到 CAP.TXT"。

所以这里用**物理键**作为真源(每个物理键 -> base 字符 / shift 字符),
生成期望映射,再和 C 表逐项比对。物理键表比展开表短一半,且不容易串行。

用法:  python tools/check_us_layout.py [firmware/io_hid/io_hid.c]
退出码 0 = 一致;1 = 有差异。
"""

import os
import re
import sys

SHIFT = 0x02

# (keycode, base_char, shift_char) —— US 布局的物理键
# 字母行 a..z 连续占用 0x04..0x1D
# 数字行的物理顺序是 1 2 3 4 5 6 7 8 9 0(不是 0 1 2 ...)
PHYSICAL_KEYS = (
    [(0x04 + i, chr(ord("a") + i), chr(ord("A") + i)) for i in range(26)]
    + [
        (0x1E, "1", "!"), (0x1F, "2", "@"), (0x20, "3", "#"),
        (0x21, "4", "$"), (0x22, "5", "%"), (0x23, "6", "^"),
        (0x24, "7", "&"), (0x25, "8", "*"), (0x26, "9", "("),
        (0x27, "0", ")"),
        (0x2C, " ", " "),
        (0x2D, "-", "_"), (0x2E, "=", "+"), (0x2F, "[", "{"),
        (0x30, "]", "}"), (0x31, "\\", "|"),
        (0x33, ";", ":"), (0x34, "'", '"'), (0x35, "`", "~"),
        (0x36, ",", "<"), (0x37, ".", ">"), (0x38, "/", "?"),
    ]
)


def expected_map():
    """生成 {char: (modifier, keycode)}。"""
    out = {}
    for key, base, shifted in PHYSICAL_KEYS:
        out[base] = (0, key)
        if shifted != base:      # 空格没有 shift 形态
            out[shifted] = (SHIFT, key)
    return out


def parse_c_table(path):
    """从 io_hid.c 里解析 s_us_map,返回 {char: (modifier, keycode)}。"""
    with open(path, "r", encoding="utf-8") as f:
        text = f.read()

    start = text.find("s_us_map[95] = {")
    if start < 0:
        raise SystemExit("FAIL: s_us_map not found in " + path)
    end = text.find("};", start)
    if end < 0:
        raise SystemExit("FAIL: s_us_map has no terminator in " + path)
    body = text[start:end]

    # 每行形如:  /* 0x20 space */ {0, 0x2C},
    row_re = re.compile(
        r"/\*\s*0x([0-9A-Fa-f]{2})\s+(.*?)\s*\*/\s*\{\s*([^,]+),\s*(0x[0-9A-Fa-f]+)\s*\}")
    out = {}
    for m in row_re.finditer(body):
        code = int(m.group(1), 16)
        mod_txt = m.group(3).strip()
        key = int(m.group(4), 16)
        if "IO_HID_MAP_SHIFT" in mod_txt:
            mod = SHIFT
        elif mod_txt == "0":
            mod = 0
        else:
            raise SystemExit("FAIL: unrecognized modifier expr: " + mod_txt)
        out[code] = (mod, key)
    return out


def main():
    repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        repo, "firmware", "io_hid", "io_hid.c")

    want = expected_map()
    got = parse_c_table(path)

    problems = []

    if len(got) != 95:
        problems.append("table has {} entries, expected 95".format(len(got)))

    for code in range(0x20, 0x7F):
        ch = chr(code)
        exp = want.get(ch)
        act = got.get(code)
        if exp is None:
            problems.append("0x{:02X} {!r}: no physical key defines it".format(code, ch))
            continue
        if act is None:
            problems.append("0x{:02X} {!r}: missing from the C table".format(code, ch))
            continue
        if act != exp:
            emod, ekey = exp
            amod, akey = act
            problems.append(
                "0x{:02X} {!r}: C table says mod=0x{:02X} key=0x{:02X}, "
                "US layout says mod=0x{:02X} key=0x{:02X}  (wrong key: 0x{:02X} is {!r})".format(
                    code, ch, amod, akey, emod, ekey, akey,
                    _describe_key(akey)))

    if problems:
        print("US layout table MISMATCH ({} problem(s)):".format(len(problems)))
        for p in problems:
            print("  " + p)
        return 1

    print("US layout table OK: 95 entries match the physical-key reference")
    return 0


def _describe_key(key):
    for k, base, shifted in PHYSICAL_KEYS:
        if k == key:
            return "{!r}/{!r}".format(base, shifted)
    return "?"


if __name__ == "__main__":
    sys.exit(main())
