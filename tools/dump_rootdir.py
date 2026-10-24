#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
dump_rootdir.py -- 解析 FAT16 镜像的根目录,打印每个目录项的原始字段。

用途:设备端 io_disk_find_file() 的判据(BPB 偏移 + 8.3 名字比对)必须
和镜像里真实的字节一致。这个脚本用**独立实现**把根目录解出来,用来
交叉核对固件里的那一套 —— 两边不一致就说明其中一边错了。

★ 为什么必须交叉核对 ★
  固件曾把"最后写入时间/日期"读成 0x24/0x26 —— 那已经越过目录项的 32
  字节,读到的是**下一个条目**的字节。解出来是 1995 年的**合法日期**,
  没有任何报错,看着还挺合理 —— 而"挑最新一帧"正是按 mtime 排的,
  于是挑帧逻辑一直错且看不出来。
  ⇒ 任何"解析出来看着挺合理"的值,都要用独立实现核一遍。

本脚本同时把三个候选偏移的时间字段都解出来,一眼就能看出哪个是真的。

用法: python tools/dump_rootdir.py [image]
默认读 firmware/build/esp-idf/io_disk/ramdisk.img
"""

import os
import struct
import sys

SEC = 512


def u16(b, off):
    return struct.unpack_from("<H", b, off)[0]


def u32(b, off):
    return struct.unpack_from("<I", b, off)[0]


def u16_at(b, off):
    """u16 that returns None instead of raising when it runs off the end."""
    if off < 0 or off + 2 > len(b):
        return None
    return struct.unpack_from("<H", b, off)[0]


def fat_time_to_str(t, d):
    """FAT 时间/日期字 -> 人话(解不出来就返回 None)。"""
    if t == 0 or d == 0:
        return None
    year = 1980 + ((d >> 9) & 0x7F)
    month = (d >> 5) & 0x0F
    day = d & 0x1F
    hour = (t >> 11) & 0x1F
    minute = (t >> 5) & 0x3F
    second = (t & 0x1F) * 2
    if not (1 <= month <= 12 and 1 <= day <= 31):
        return None
    return "%04d-%02d-%02d %02d:%02d:%02d" % (year, month, day, hour, minute, second)


def main():
    repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        repo, "firmware", "build", "esp-idf", "io_disk", "ramdisk.img")
    with open(path, "rb") as f:
        img = f.read()

    bps = u16(img, 11)
    spc = img[13]
    reserved = u16(img, 14)
    num_fats = img[16]
    root_entries = u16(img, 17)
    fat_size = u16(img, 22)
    total32 = u32(img, 32)

    root_lba = reserved + num_fats * fat_size
    data_lba = root_lba + (root_entries * 32 + SEC - 1) // SEC

    print("image      : %s (%d bytes)" % (path, len(img)))
    print("bytes/sect : %d" % bps)
    print("sect/clus  : %d" % spc)
    print("reserved   : %d" % reserved)
    print("num FATs   : %d" % num_fats)
    print("FAT size   : %d sectors" % fat_size)
    print("root ent   : %d  (FAT16 根目录项数有限,不能靠'每次写个新文件名'做标记)" % root_entries)
    print("total32    : %d sectors" % total32)
    print("root  LBA  : %d  (byte offset %d)" % (root_lba, root_lba * SEC))
    print("data  LBA  : %d" % data_lba)
    print("FAT sig    : %r" % img[0x36:0x42])
    print("")

    fat = img[reserved * SEC: (reserved + fat_size) * SEC]
    off = root_lba * SEC
    for i in range(root_entries):
        e = img[off + i * 32: off + i * 32 + 32]
        if e[0] == 0x00:
            print("  [%02d] end-of-entries" % i)
            break
        if e[0] == 0xE5:
            print("  [%02d] <deleted>" % i)
            continue
        name = e[0:11]
        attr = e[11]
        first = u16(e, 26)
        size = u32(e, 28)
        # 簇链
        chain = []
        c = first
        while 2 <= c < 0xFFF8 and len(chain) < 12:
            chain.append(c)
            c = struct.unpack_from("<H", fat, c * 2)[0]
        print("  [%02d] raw_name=%r  attr=0x%02X  first_cluster=%d  size=%d  chain=%s"
              % (i, bytes(name), attr, first, size, chain))
        if attr & 0x08:
            print("        (volume label)")
            continue
        for t_off, d_off, label in (
                (0x16, 0x18, "write time/date  <- FAT16 正确偏移"),
                (0x24, 0x26, "0x24/0x26        <- 越过本条目(旧代码的错误偏移)"),
                (0x0E, 0x10, "creation time/date")):
            # 注意:0x24/0x26 已经越过本条目 32 字节,读到的其实是**下一个
            # 条目**的字节 —— 这正是旧代码读出来的垃圾。所以这里刻意按
            # "越界就读下一条目"的方式复现它,好让差异一眼可见。
            base = off + i * 32
            t = u16_at(img, base + t_off)
            d = u16_at(img, base + d_off)
            if t is None or d is None:
                print("        %-38s (out of the image)" % label)
                continue
            print("        %-38s raw=%04X/%04X  -> %s"
                  % (label, t, d, fat_time_to_str(t, d) or "-"))


if __name__ == "__main__":
    sys.exit(main())
