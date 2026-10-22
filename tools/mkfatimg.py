#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
mkfatimg.py — 生成 ESP32 虚拟 U 盘(RAM disk)用的 FAT16 镜像。

为什么要有这个工具:
  USB MSC 是"块设备",Windows 自己负责 FAT 文件系统的一切(分配簇、
  写目录项、更新 FAT)。设备端只需要一块**格式正确的 FAT16 块设备**。
  所以 FAT 的正确性全部落在本工具上——BPB 参数写错,Windows 会提示
  "需要格式化"甚至根本不认卷(见 docs/implementation-plan.md M1 风险点)。

布局(默认 4MB / 512B 扇区 / 1 扇区每簇):
    扇区 0        : 引导扇区(BPB)
    扇区 1..32    : FAT1
    扇区 33..64   : FAT2
    扇区 65..96   : 根目录(512 项)
    扇区 97..     : 数据区(cluster 2 起)

FAT16 硬约束:簇数必须落在 [4085, 65524],否则 Windows 不按 FAT16 挂载。
4MB / 1 扇区每簇 → 8095 簇,合法。

稀疏输出(默认开启,--dense 关闭):
    **卷容量由 BPB 声明,不由镜像文件长度决定。**
    4MB 的卷里通常只有几十 KB 预置文件,剩下的数据区出厂时是空的。
    把空数据区写进固件就是纯浪费:4MB 镜像里 99.5% 是零。
    所以镜像只输出到"最后一个已用扇区",BPB 的 total32 仍然写完整扇区数。
    固件启动时按 total32 在 PSRAM 里开满卷,尾部补零 —— 设备端依然零 FAT 逻辑,
    宿主看到的仍然是一个完整的 4MB 可移动卷。
    实际效果:内嵌镜像 4MB → 约 50KB,app 分区(OTA 槽)随之缩小一个数量级。

预留槽位(--reserve-slot / --reserve-inbox,T2):
    运行时下发的引导文件(WATCH.PS1 / AGENT.SH / ...)在出厂镜像里必须**占好位置**:
    簇链要在 FAT 里标为已分配(否则宿主 FAT 驱动会把这些簇分给别的文件),
    目录项要在(宿主要按名字打开它)。但**槽位容量不该花固件空间** ——
    AI 现场就能生成这些文件,固件只该占"位置",不该占"内容"。

    做法:把预留槽位的簇**从卷末尾往前分配**(普通文件从卷开头往后分配),
    簇链和目录项写进 FAT/根目录区(镜像里),而簇里的**内容不写进镜像文件**。
    镜像文件因此停在元数据区末尾,固件里每个预留槽位只花几十字节元数据,
    槽位容量(几百 KB)在固件里是 **0 字节**。
    固件启动时按 total32 开满卷,尾部补零 —— 预留槽位出厂时就是一段零。

    两种槽位:
      --reserve-slot  NAME:CAP   设备管理的槽(由固件写入并改目录项 size),
                                 出厂 size=0,CAP 是最大 payload 长度
      --reserve-inbox NAME:CAP   宿主写的信箱(固件只读),出厂 size=CAP,
                                 宿主必须按同样大小覆写才能保持簇链不变

用法:
    python tools/mkfatimg.py --out firmware/assets/ramdisk.img \
        --label IOAGENT --size 4M \
        --reserve-slot WATCH.PS1:32768 \
        --reserve-inbox PAYLOAD.BIN:131072 \
        --emit-slot-table firmware/build/ramdisk_slots.h
    python tools/mkfatimg.py --verify firmware/assets/ramdisk.img
"""

import argparse
import os
import struct
import sys
import time

SECTOR = 512
FAT16_MIN_CLUSTERS = 4085
FAT16_MAX_CLUSTERS = 65524
ATTR_LFN = 0x0F
ATTR_DIR = 0x10
ATTR_ARC = 0x20
ATTR_VOLUME = 0x08

# 8.3 短名合法性
VALID_CHARS = set(
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
    "!#$%&'()-@^_`{}~"
)


class Fat16Error(Exception):
    pass


def to_8_3(name):
    """把 'watch.ps1' 规范成 8.3 短名 (base, ext)。"""
    if "." in name:
        base, ext = name.rsplit(".", 1)
    else:
        base, ext = name, ""
    base = base.upper().replace(" ", "")
    ext = ext.upper().replace(" ", "")
    if not base or len(base) > 8 or len(ext) > 3:
        raise Fat16Error(f"文件名不符合 8.3 短名: {name!r}")
    for ch in base + ext:
        if ch not in VALID_CHARS:
            raise Fat16Error(f"文件名含 8.3 不允许的字符: {name!r} ({ch!r})")
    return base, ext


def fat16_time(dt=None):
    dt = dt or time.localtime()
    return ((dt.tm_hour << 11) | (dt.tm_min << 5) | (dt.tm_sec // 2), 
            (((dt.tm_year - 1980) << 9) | (dt.tm_mon << 5) | dt.tm_mday))


class Fat16Image:
    def __init__(self, size_bytes=4 * 1024 * 1024, label="IOAGENT", serial=None):
        if size_bytes % SECTOR:
            raise Fat16Error("卷大小必须是扇区大小(512)的整数倍")
        self.total_sectors = size_bytes // SECTOR
        self.label = label[:11].ljust(11)
        self.serial = serial if serial is not None else (0x12345678 & 0xFFFFFFFF)

        # 预留 1 扇区引导 + 2 个 FAT + 根目录 512 项(32 扇区),其余为数据区。
        # 簇数 = 数据扇区数(1 扇区/簇);FAT 大小要能装下簇数+2 个 16 位项。
        reserved = 1
        fat_count = 2
        root_entries = 512
        root_sectors = (root_entries * 32 + SECTOR - 1) // SECTOR

        # 先按 "1 扇区/簇" 估 FAT 大小,再迭代求自洽解
        fat_sectors = 0
        for _ in range(64):
            data_sectors = self.total_sectors - reserved - fat_count * fat_sectors - root_sectors
            clusters = data_sectors
            need = ((clusters + 2) * 2 + SECTOR - 1) // SECTOR
            if need == fat_sectors:
                break
            fat_sectors = need
        else:
            raise Fat16Error("FAT 大小迭代未收敛")

        data_sectors = self.total_sectors - reserved - fat_count * fat_sectors - root_sectors
        clusters = data_sectors
        if not (FAT16_MIN_CLUSTERS <= clusters <= FAT16_MAX_CLUSTERS):
            raise Fat16Error(
                f"簇数 {clusters} 不在 FAT16 合法区间 "
                f"[{FAT16_MIN_CLUSTERS}, {FAT16_MAX_CLUSTERS}] —— "
                f"请调整 --size(当前 {size_bytes} 字节)"
            )

        self.reserved = reserved
        self.fat_count = fat_count
        self.fat_sectors = fat_sectors
        self.root_entries = root_entries
        self.root_sectors = root_sectors
        self.sectors_per_cluster = 1
        self.cluster_count = clusters

        self.root_start = reserved + fat_count * fat_sectors
        self.data_start = self.root_start + root_sectors

        # FAT 内容(只维护 FAT1,FAT2 是它的镜像)
        self.fat = [0x0000] * (clusters + 2)  # 0,1 保留
        self.fat[0] = 0xFFF8
        self.fat[1] = 0xFFFF

        # 根目录项(32 字节),第 0 项是卷标
        self.root = bytearray()
        self._add_volume_entry()

        # 数据区:簇号 -> bytes
        self.files = {}   # name -> (start_cluster, size)
        self._cluster_data = {}
        self._next_free = 2
        # 预留槽位从卷末尾往前分配(_top_free 递减),普通文件从卷开头往后分配。
        # 两个分配器不相遇 => 槽位簇永远落在"镜像文件长度之外" => 槽位容量不占固件。
        self._top_free = self.cluster_count + 1
        # 预留槽位表:name -> dict
        self.slots = {}

    def _add_volume_entry(self):
        label = self.label.strip()
        ent = bytearray(32)
        ent[0:11] = label.encode("ascii", "replace").ljust(11)[:11]
        ent[11] = ATTR_VOLUME
        self.root += ent

    def _alloc_cluster(self):
        if self._next_free > self.cluster_count + 1:
            raise Fat16Error("磁盘空间不足")
        if self._next_free > self._top_free:
            raise Fat16Error("磁盘空间不足(与预留槽位冲突)")
        c = self._next_free
        self._next_free += 1
        self.fat[c] = 0xFFFF
        return c

    def _alloc_clusters_top(self, count):
        """从卷末尾往前分配 count 个簇,返回升序列表。"""
        if count > (self._top_free - 1):
            raise Fat16Error("磁盘空间不足(预留槽位超出容量)")
        clusters = []
        for _ in range(count):
            c = self._top_free
            self._top_free -= 1
            self.fat[c] = 0xFFFF
            clusters.append(c)
        clusters.sort()
        return clusters

    def add_file(self, name, data):
        """把文件放进根目录(单层,不做子目录)。同名则覆盖。"""
        base, ext = to_8_3(name)
        key = base + "." + ext if ext else base
        if key in self.files:
            raise Fat16Error(f"重复文件: {key}")

        clusters = []
        off = 0
        while off < len(data):
            c = self._alloc_cluster()
            clusters.append(c)
            chunk = data[off:off + SECTOR]
            chunk = chunk.ljust(SECTOR, b"\x00")
            self._cluster_data[c] = chunk
            off += SECTOR
        if not clusters:
            c = self._alloc_cluster()
            clusters = [c]
            self._cluster_data[c] = b"\x00" * SECTOR

        for i, c in enumerate(clusters[:-1]):
            self.fat[c] = clusters[i + 1]
        self.fat[clusters[-1]] = 0xFFFF

        tm, dt = fat16_time()
        ent = bytearray(32)
        ent[0:8] = base.encode("ascii").ljust(8)[:8]
        ent[8:11] = ext.encode("ascii").ljust(3)[:3] if ext else b"   "
        ent[11] = ATTR_ARC
        ent[12] = 0                                   # NTRes
        struct.pack_into("<H", ent, 14, tm)          # CrtTime
        struct.pack_into("<H", ent, 16, dt)          # CrtDate
        struct.pack_into("<H", ent, 18, dt)          # LstAccDate
        start = clusters[0]
        struct.pack_into("<H", ent, 20, (start >> 16) & 0xFFFF)   # FstClusHI
        struct.pack_into("<H", ent, 22, tm)          # WrtTime
        struct.pack_into("<H", ent, 24, dt)          # WrtDate
        struct.pack_into("<H", ent, 26, start & 0xFFFF)          # FstClusLO
        struct.pack_into("<I", ent, 28, len(data))  # FileSize
        self.root += ent
        self.files[key] = (start, len(data))
        return key

    def reserve_file(self, name, capacity, kind="slot"):
        """预留一个运行时槽位:占好簇链和目录项,内容不写进镜像文件。

        kind == "slot"  : 设备管理。出厂 size=0,固件写入 payload 后改 size。
        kind == "inbox" : 宿主写。出厂 size=capacity,宿主按同样大小覆写。

        簇从卷末尾往前分配,所以这些簇落在"镜像文件长度之外" ——
        槽位容量在固件里是 0 字节,只有 FAT 链项(2B/簇)和目录项(32B)进镜像。
        """
        if capacity <= 0:
            raise Fat16Error(f"槽位容量必须为正:{name!r}")
        base, ext = to_8_3(name)
        key = base + "." + ext if ext else base
        if key in self.files or key in self.slots:
            raise Fat16Error(f"重复文件: {key}")

        per_cluster = SECTOR * self.sectors_per_cluster
        n_clusters = (capacity + per_cluster - 1) // per_cluster
        clusters = self._alloc_clusters_top(n_clusters)
        for i, c in enumerate(clusters[:-1]):
            self.fat[c] = clusters[i + 1]
        self.fat[clusters[-1]] = 0xFFFF

        size = 0 if kind == "slot" else capacity
        tm, dt = fat16_time()
        entry_index = len(self.root) // 32
        ent = bytearray(32)
        ent[0:8] = base.encode("ascii").ljust(8)[:8]
        ent[8:11] = ext.encode("ascii").ljust(3)[:3] if ext else b"   "
        ent[11] = ATTR_ARC
        ent[12] = 0
        struct.pack_into("<H", ent, 14, tm)
        struct.pack_into("<H", ent, 16, dt)
        struct.pack_into("<H", ent, 18, dt)
        start = clusters[0]
        struct.pack_into("<H", ent, 20, (start >> 16) & 0xFFFF)
        struct.pack_into("<H", ent, 22, tm)
        struct.pack_into("<H", ent, 24, dt)
        struct.pack_into("<H", ent, 26, start & 0xFFFF)
        struct.pack_into("<I", ent, 28, size)
        self.root += ent

        info = {
            "name": key,
            "kind": kind,
            "start_cluster": start,
            "clusters": n_clusters,
            "capacity": capacity,
            "reserved": n_clusters * per_cluster,
            "data_offset": (self.data_start + (start - 2) * self.sectors_per_cluster) * SECTOR,
            "size_field_offset": self.root_start * SECTOR + entry_index * 32 + 28,
        }
        self.slots[key] = info
        self.files[key] = (start, size)
        return key

    def emit_slot_table(self, path):
        """导出槽位表(.h + .c)。.h 只有类型和 extern 声明,任何 .c 都能包含。"""
        lines = []
        lines.append("/* AUTO-GENERATED by tools/mkfatimg.py --emit-slot-table. DO NOT EDIT. */")
        lines.append("#pragma once")
        lines.append("#include <stdint.h>")
        lines.append("")
        lines.append("#define IO_SLOT_KIND_SLOT  0u")
        lines.append("#define IO_SLOT_KIND_INBOX 1u")
        lines.append("")
        lines.append("typedef struct {")
        lines.append("    const char *name;             /* 8.3 short name, host-visible */")
        lines.append("    uint32_t    kind;             /* IO_SLOT_KIND_* */")
        lines.append("    uint32_t    data_offset;      /* byte offset of first data cluster in the volume */")
        lines.append("    uint32_t    capacity;         /* max payload bytes */")
        lines.append("    uint32_t    reserved;         /* cluster-aligned bytes actually reserved */")
        lines.append("    uint32_t    size_field_offset;/* byte offset of the 4-byte FileSize field */")
        lines.append("    uint32_t    start_cluster;")
        lines.append("} io_slot_desc_t;")
        lines.append("")
        n = len(self.slots)
        lines.append("#define IO_SLOT_COUNT %du" % n)
        lines.append("")
        lines.append("extern const io_slot_desc_t io_slot_table[IO_SLOT_COUNT];")
        lines.append("")
        os.makedirs(os.path.dirname(os.path.abspath(path)) or ".", exist_ok=True)
        with open(path, "w", encoding="ascii", newline="\n") as f:
            f.write("\n".join(lines))

        cpath = os.path.splitext(path)[0] + ".c"
        clines = []
        clines.append("/* AUTO-GENERATED by tools/mkfatimg.py --emit-slot-table. DO NOT EDIT. */")
        clines.append('#include "ramdisk_slots.h"')
        clines.append("")
        clines.append("const io_slot_desc_t io_slot_table[IO_SLOT_COUNT] = {")
        for key, info in self.slots.items():
            clines.append(
                '    { "%s", %du, %du, %du, %du, %du, %du },'
                % (
                    key,
                    0 if info["kind"] == "slot" else 1,
                    info["data_offset"],
                    info["capacity"],
                    info["reserved"],
                    info["size_field_offset"],
                    info["start_cluster"],
                )
            )
        clines.append("};")
        clines.append("")
        with open(cpath, "w", encoding="ascii", newline="\n") as f:
            f.write("\n".join(clines))
        return path, cpath, n

    def embed_sector_count(self, sparse=True):
        """镜像文件实际写出的扇区数。

        稀疏:只到"最后一个已用扇区"(数据区末尾)。卷容量仍由 BPB total32 声明,
        固件按 total32 在 PSRAM 里开满卷并把尾部补零。
        稠密:写满 total_sectors。
        """
        if not sparse:
            return self.total_sectors
        # 根目录区必须完整存在(目录项区),数据区只到最后一个被分配的簇。
        last = self.root_start + self.root_sectors - 1
        for c in self._cluster_data:
            end = (self.data_start + (c - 2) * self.sectors_per_cluster
                   + self.sectors_per_cluster - 1)
            if end > last:
                last = end
        return min(self.total_sectors, last + 1)

    def build(self, path, sparse=True):
        embed_sectors = self.embed_sector_count(sparse)
        img = bytearray(embed_sectors * SECTOR)

        # 引导扇区
        bpb = bytearray(SECTOR)
        bpb[0:3] = b"\xEB\x3C\x90"
        bpb[3:11] = b"MSWIN4.1"
        struct.pack_into("<H", bpb, 11, SECTOR)
        bpb[13] = self.sectors_per_cluster
        struct.pack_into("<H", bpb, 14, self.reserved)
        bpb[16] = self.fat_count
        struct.pack_into("<H", bpb, 17, self.root_entries)
        struct.pack_into("<H", bpb, 19, 0)              # total16 = 0 → 用 32 位字段
        bpb[21] = 0xF8                                # media descriptor
        struct.pack_into("<H", bpb, 22, self.fat_sectors)
        struct.pack_into("<H", bpb, 24, 63)            # sectors per track
        struct.pack_into("<H", bpb, 26, 255)           # heads
        struct.pack_into("<I", bpb, 28, 0)             # hidden sectors
        struct.pack_into("<I", bpb, 32, self.total_sectors)
        bpb[36] = 0x80                               # drive number
        bpb[38] = 0x29                               # extended boot signature
        struct.pack_into("<I", bpb, 39, self.serial)
        bpb[43:54] = self.label.encode("ascii", "replace").ljust(11)[:11]
        bpb[54:62] = b"FAT16   "
        bpb[510:512] = b"\x55\xAA"
        img[0:SECTOR] = bpb

        # FAT
        fat_bytes = bytearray((self.cluster_count + 2) * 2)
        for i, v in enumerate(self.fat):
            struct.pack_into("<H", fat_bytes, i * 2, v)
        for i in range(self.fat_count):
            off = (self.reserved + i * self.fat_sectors) * SECTOR
            img[off:off + len(fat_bytes)] = fat_bytes

        # 根目录
        root_off = self.root_start * SECTOR
        img[root_off:root_off + len(self.root)] = self.root

        # 数据区
        for c, data in self._cluster_data.items():
            off = (self.data_start + (c - 2) * self.sectors_per_cluster) * SECTOR
            img[off:off + len(data)] = data

        os.makedirs(os.path.dirname(os.path.abspath(path)) or ".", exist_ok=True)
        with open(path, "wb") as f:
            f.write(img)
        return path, embed_sectors


def verify(path):
    """回读镜像,校验 BPB 与文件可还原性。返回 (ok, 报告行)。"""
    lines = []
    ok = True
    with open(path, "rb") as f:
        img = f.read()
    bpb = img[:SECTOR]
    bps = struct.unpack_from("<H", bpb, 11)[0]
    spc = bpb[13]
    reserved = struct.unpack_from("<H", bpb, 14)[0]
    nfat = bpb[16]
    root_entries = struct.unpack_from("<H", bpb, 17)[0]
    fat_sectors = struct.unpack_from("<H", bpb, 22)[0]
    total32 = struct.unpack_from("<I", bpb, 32)[0]
    sig = bpb[510:512]
    label = bpb[43:54].decode("ascii", "replace").strip()
    fs_type = bpb[54:62].decode("ascii", "replace").strip()

    def chk(cond, msg):
        nonlocal ok
        lines.append(("PASS " if cond else "FAIL ") + msg)
        if not cond:
            ok = False

    chk(bps == 512, f"bytes/sector = {bps}")
    chk(spc == 1, f"sectors/cluster = {spc}")
    chk(nfat == 2, f"FAT 数 = {nfat}")
    chk(sig == b"\x55\xAA", f"引导签名 = {sig.hex()}")
    chk(fs_type == "FAT16", f"文件系统标识 = {fs_type!r}")
    embedded = len(img) // SECTOR
    chk(len(img) == embedded * SECTOR, f"镜像长度 = {len(img)} 字节 (整扇区)")
    chk(embedded <= total32,
        f"total32 = {total32} (镜像内嵌 {embedded} 扇区)")
    if embedded < total32:
        lines.append("INFO 稀疏镜像: 卷容量 {0} 扇区 ({1} KB), 内嵌 {2} 扇区 ({3} KB), "
                     "固件在 PSRAM 里补零 {4} 扇区".format(
                         total32, total32 * SECTOR // 1024,
                         embedded, embedded * SECTOR // 1024,
                         total32 - embedded))
    else:
        lines.append("INFO 稠密镜像: 内嵌扇区数 = 卷容量 ({0})".format(total32))

    root_sectors = (root_entries * 32 + 511) // 512
    data_start = reserved + nfat * fat_sectors + root_sectors
    clusters = total32 - data_start
    chk(FAT16_MIN_CLUSTERS <= clusters <= FAT16_MAX_CLUSTERS,
        f"簇数 = {clusters} (合法区间 {FAT16_MIN_CLUSTERS}..{FAT16_MAX_CLUSTERS})")

    # 解析 FAT1
    fat1 = img[reserved * 512: (reserved + fat_sectors) * 512]
    fat = list(struct.unpack_from("<%dH" % (len(fat1) // 2), fat1, 0))
    # 解析根目录
    root = img[(reserved + nfat * fat_sectors) * 512:
               (reserved + nfat * fat_sectors) * 512 + root_entries * 32]
    found = 0
    for i in range(root_entries):
        ent = root[i * 32:(i + 1) * 32]
        attr = ent[11]
        if attr in (0x00, ATTR_VOLUME) or attr & ATTR_LFN:
            continue
        name = ent[0:8].decode("ascii", "replace").strip()
        ext = ent[8:11].decode("ascii", "replace").strip()
        size = struct.unpack_from("<I", ent, 28)[0]
        start = struct.unpack_from("<H", ent, 26)[0] | (struct.unpack_from("<H", ent, 20)[0] << 16)
        # 走簇链
        data = b""
        c = start
        seen = set()
        while 2 <= c < len(fat) and c not in seen:
            seen.add(c)
            off = (data_start + (c - 2) * spc) * 512
            chunk = img[off:off + spc * 512]
            # 稀疏镜像:预留槽位的簇在镜像长度之外 -> 内容在 RAM 里是零
            chunk = chunk.ljust(spc * 512, b"\x00")
            data += chunk
            c = fat[c]
        fname = name + ("." + ext if ext else "")
        good = (len(data[:size]) == size) and (c == 0xFFFF or c == 0xFFF7)
        chk(good, f"文件 {fname}: size={size} 起始簇={start} 链尾=0x{c:04X}")
        found += 1
    lines.append(f"INFO 根目录文件数 = {found}")
    return ok, lines


def parse_size(text):
    """'32768' / '32K' / '128K' -> 字节数。"""
    s = str(text).strip().upper()
    mult = {"K": 1024, "M": 1024 * 1024, "G": 1024 ** 3}
    if s and s[-1] in mult:
        return int(float(s[:-1]) * mult[s[-1]])
    return int(s)


def main():
    ap = argparse.ArgumentParser(description="生成/校验 FAT16 虚拟 U 盘镜像")
    ap.add_argument("--out", help="输出镜像路径")
    ap.add_argument("--verify", metavar="IMG", help="校验已有镜像")
    ap.add_argument("--label", default="IOAGENT")
    ap.add_argument("--size", default="4M", help="卷大小,支持 K/M/G")
    ap.add_argument("--add", action="append", default=[],
                    metavar="SRC:DSTNAME", help="加入文件,可重复")
    ap.add_argument("--reserve-slot", action="append", default=[],
                    metavar="NAME:CAP", help="预留设备管理槽位(出厂 size=0),可重复")
    ap.add_argument("--reserve-inbox", action="append", default=[],
                    metavar="NAME:CAP", help="预留宿主信箱(出厂 size=CAP),可重复")
    ap.add_argument("--emit-slot-table", metavar="HEADER",
                    help="导出槽位表 C 头文件")
    ap.add_argument("--dense", action="store_true",
                    help="写满整个卷(默认稀疏:只输出到最后一个已用扇区)")
    args = ap.parse_args()

    if args.verify:
        ok, lines = verify(args.verify)
        for ln in lines:
            print(ln)
        print("RESULT:", "OK" if ok else "FAILED")
        return 0 if ok else 1

    if not args.out:
        ap.error("需要 --out 或 --verify")

    size = parse_size(args.size)

    img = Fat16Image(size_bytes=size, label=args.label)
    for spec in args.add:
        if ":" not in spec:
            ap.error(f"--add 格式应为 SRC:DSTNAME,得到 {spec!r}")
        # 必须按**最后一个**冒号切:Windows 绝对路径自带盘符冒号(D:\...),
        # 按第一个冒号切会把盘符当成源文件路径。
        src, dst = spec.rsplit(":", 1)
        with open(src, "rb") as f:
            data = f.read()
        key = img.add_file(dst, data)
        print(f"ADD {src} -> {key} ({len(data)} 字节)")

    for kind, specs in (("slot", args.reserve_slot), ("inbox", args.reserve_inbox)):
        for spec in specs:
            if ":" not in spec:
                ap.error(f"槽位格式应为 NAME:CAP,得到 {spec!r}")
            name, cap = spec.rsplit(":", 1)
            key = img.reserve_file(name, parse_size(cap), kind=kind)
            info = img.slots[key]
            print("RESERVE {0} kind={1} capacity={2} reserved={3} start_cluster={4} "
                  "data_offset={5} size_field_offset={6}".format(
                      key, kind, info["capacity"], info["reserved"],
                      info["start_cluster"], info["data_offset"],
                      info["size_field_offset"]))

    path, embed_sectors = img.build(args.out, sparse=not args.dense)
    print("WROTE {0} ({1} 字节, 簇数 {2}, 内嵌 {3} 扇区 / 卷 {4} 扇区)".format(
        path, os.path.getsize(path), img.cluster_count, embed_sectors, img.total_sectors))

    if args.emit_slot_table:
        tbl, tblc, n = img.emit_slot_table(args.emit_slot_table)
        print("SLOTTABLE {0} + {1} ({2} slots)".format(tbl, tblc, n))

    ok, lines = verify(path)
    for ln in lines:
        print(ln)
    print("RESULT:", "OK" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
