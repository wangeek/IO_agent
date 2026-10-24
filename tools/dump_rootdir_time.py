#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Read the live volume's raw root directory and show where the timestamps are.

The device reads the FAT16 directory entry to get "last write time".
This script reads the SAME bytes straight off the volume, so we can see
which offsets actually carry a plausible timestamp.
"""
import os
import struct
import sys

SEC = 512
VOL = sys.argv[1] if len(sys.argv) > 1 else r"\\.\E:"


def u16(b, o):
    return struct.unpack_from("<H", b, o)[0]


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def decode(t, d):
    """FAT time/date word -> human string (or None)."""
    if d == 0 or t == 0:
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
    with open(VOL, "rb") as f:
        boot = f.read(SEC)
        reserved = u16(boot, 14)
        num_fats = boot[16]
        root_entries = u16(boot, 17)
        fat_size = u16(boot, 22)
        root_lba = reserved + num_fats * fat_size
        f.seek(root_lba * SEC)
        root = f.read(root_entries * 32)

    print("volume      : %s" % VOL)
    print("root LBA    : %d   root entries: %d" % (root_lba, root_entries))
    print("")

    for i in range(root_entries):
        e = root[i * 32:(i + 1) * 32]
        if e[0] == 0x00:
            break
        if e[0] == 0xE5:
            continue
        name = bytes(e[0:11]).decode("latin-1")
        attr = e[11]
        if attr & 0x08:
            continue
        print("[%02d] %r attr=0x%02X size=%d" % (i, name, attr, u32(e, 0x1C)))
        # candidate offsets for the "last write time/date" pair
        for t_off, d_off, label in ((0x16, 0x18, "0x16/0x18  <- FAT16 write time/date"),
                                    (0x24, 0x26, "0x24/0x26  <- what io_disk.c reads"),
                                    (0x0E, 0x10, "0x0E/0x10  <- creation time/date")):
            t = u16(e, t_off)
            d = u16(e, d_off)
            print("      %-34s raw=%04X/%04X  -> %s"
                  % (label, t, d, decode(t, d) or "-"))
        print("      raw bytes 0x00..0x1F: %s" % bytes(e[0:32]).hex())


if __name__ == "__main__":
    main()
