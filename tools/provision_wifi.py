#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""provision_wifi.py -- 把 WiFi 凭据写进设备(宿主写 U 盘,设备自己取)。

设备端在开机时、以及之后每 5 秒,会去卷上找 `DEV.CFG`;内容变了就写进 NVS
并重连。所以这条通道是"宿主写 -> 设备读",方向天然对 —— 设备还没联网的时候
agent 连不上它,只有这条路。

DEV.CFG 是行式 key=value:
    ssid=MyNetwork
    pass=secret123        # 可省略 -> 沿用设备里已有的密码;写 pass= 空 = 开放网络
    name=sophon-lab      # 可省略

用法:
    python tools/provision_wifi.py --ssid MyNetwork --pass secret
    python tools/provision_wifi.py --ssid MyNetwork          # 沿用已有密码
    python tools/provision_wifi.py --show                    # 只看盘上有什么
"""

import argparse
import os
import sys
import time

LABEL = "SOPHON"
CFG_NAME = "DEV.CFG"


def find_volume_root(label=LABEL):
    """按卷标找可移动卷的根(和 watcher/watch.ps1 里的 Get-VolumeRoot 同逻辑)。"""
    import ctypes

    class STORAGEPROPSTRUCT(ctypes.Structure):
        _fields_ = [
            ("dwPropsID", ctypes.c_ulong), ("cbSize", ctypes.c_ulong),
            ("cchFileSystemName", ctypes.c_ulong),
            ("cchVolumeName", ctypes.c_ulong),
            ("cchVolumeLabel", ctypes.c_ulong),
            ("cchFileSystem", ctypes.c_ulong),
            ("cchXAttr", ctypes.c_ulong),
            ("VolumeLabel", ctypes.c_ulong * 256),
            ("VolumeName", ctypes.c_ulong * 260),
            ("FileSystem", ctypes.c_ulong * 260),
        ]

    kernel32 = ctypes.windll.kernel32
    # FindFirstVolumeW / FindNextVolumeW / FindVolumeClose
    FindFirstVolumeW = kernel32.FindFirstVolumeW
    FindFirstVolumeW.restype = ctypes.c_void_p
    FindFirstVolumeW.argtypes = [ctypes.c_wchar_p, ctypes.c_ulong]
    FindNextVolumeW = kernel32.FindNextVolumeW
    FindNextVolumeW.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p, ctypes.c_ulong]
    FindVolumeClose = kernel32.FindVolumeClose
    FindVolumeClose.argtypes = [ctypes.c_void_p]
    GetVolumeInformationW = kernel32.GetVolumeInformationW
    GetVolumeInformationW.argtypes = [ctypes.c_wchar_p, ctypes.c_wchar_p, ctypes.c_ulong,
                                      ctypes.POINTER(ctypes.c_ulong),
                                      ctypes.POINTER(ctypes.c_ulong),
                                      ctypes.POINTER(ctypes.c_ulong),
                                      ctypes.c_wchar_p, ctypes.c_ulong]

    buf = ctypes.create_unicode_buffer(1024)
    find = FindFirstVolumeW(buf, ctypes.sizeof(buf))
    if not find:
        return None
    try:
        while True:
            root = buf.value
            namebuf = ctypes.create_unicode_buffer(261)
            fsbuf = ctypes.create_unicode_buffer(261)
            serial = ctypes.c_ulong()
            maxcomp = ctypes.c_ulong()
            flags = ctypes.c_ulong()
            if GetVolumeInformationW(root, namebuf, 261, ctypes.byref(serial),
                                     ctypes.byref(maxcomp), ctypes.byref(flags),
                                     fsbuf, 260):
                if namebuf.value == label and fsbuf.value.upper() in ("FAT", "FAT32", "EXFAT"):
                    return root
            if not FindNextVolumeW(find, buf, ctypes.sizeof(buf)):
                break
    finally:
        FindVolumeClose(find)
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ssid")
    ap.add_argument("--pass", dest="password")
    ap.add_argument("--name")
    ap.add_argument("--label", default=LABEL)
    ap.add_argument("--show", action="store_true", help="只列盘上的文件")
    args = ap.parse_args()

    root = find_volume_root(args.label)
    if not root:
        print("volume %r not found (is the device plugged in?)" % args.label)
        return 2
    print("volume root: %s" % root)

    if args.show:
        for name in sorted(os.listdir(root)):
            p = os.path.join(root, name)
            try:
                size = os.path.getsize(p)
            except OSError:
                size = -1
            print("  %-14s %d" % (name, size))
        return 0

    if not args.ssid:
        print("need --ssid (or --show)")
        return 2

    lines = ["# sophon provisioning", "ssid=%s" % args.ssid]
    if args.password is not None:
        lines.append("pass=%s" % args.password)
    if args.name:
        lines.append("name=%s" % args.name)
    body = ("\r\n".join(lines) + "\r\n").encode("ascii", "replace")

    path = os.path.join(root, CFG_NAME)
    with open(path, "wb") as f:
        f.write(body)
        f.flush()
        os.fsync(f.fileno())
    print("wrote %s (%d bytes) -- the device picks it up within ~5 s" % (path, len(body)))
    print("(the password stays on the device's RAM disk; delete the file when you like)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
