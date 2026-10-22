#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
dump_rootdir.py -- 解析 FAT16 镜像的根目录,打印每个目录项的原始字段。

用途:设备端 io_disk_find_file() 的判据(BPB 偏移 + 8.3 名字比对)必须
和镜像里真实的字节一致。这个脚本用**独立实现**把根目录解出来,用来
交叉核对固件里的那一套 —— 两边不一致就说明其中一边错了。

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
    print("root ent   : %d" % root_entries)
    print("total32    : %d sectors" % total32)
    print("root  LBA  : %d  (byte offset %d)" % (root_lba, root_lba * SEC))
    print("data  LBA  : %d" % data_lba)
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


if __name__ == "__main__":
    sys.exit(main())
