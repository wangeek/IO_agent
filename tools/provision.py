#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""provision.py — 给 Sophon 设备写配网文件(往共享盘上放 DEV.CFG)。

为什么需要它
------------
新板子(单 Type-C dongle)只有**一个** USB 口,插在目标机上就没有第二个口
给开发机了 ⇒:

  · **烧录**:只能产线上用 USB 直烧(ESP32-S3 的 USB-OTG 支持
    USB-Serial-JTAG ROM 下载模式,esptool 直接可用);
  · **配网**:没有 UART 控制台可用(控制台在 UART0 上)⇒ 唯一方向天然对的
    通道是 **U 盘**:宿主把 `DEV.CFG` 写到共享盘,设备自己取走
    (设备开机取一次,之后每 5 秒再看一眼,内容变了就应用)。

本脚本做第二步:**按卷标找到那个可移动卷,把 DEV.CFG 写进去**。

★ 为什么必须**按卷标**而不是盘符 ★
  dongle 拔线即断电,重入是冷启动,Windows 可能分配到**别的盘符**
  ⇒ 写死 X:\ 必然在重入后失效。卷标是稳定的。

用法
----
    python tools/provision.py --ssid MyNet --pass secret123
    python tools/provision.py --ssid MyNet --pass secret123 --name sophon-lab
    python tools/provision.py --ssid MyNet --pass secret123 --log-ips 172.17.20.17
    python tools/provision.py --dry-run          # 只打印要写什么,不落盘
    python tools/provision.py --list-volumes     # 列出可移动卷

设备侧会打印:
    provisioning applied from DEV.CFG: ssid="..." pass=<set> name=... logips=...
"""

import argparse
import json
import os
import subprocess
import sys

VOLUME_LABEL = "SOPHON"      # 设备共享盘的卷标(见 firmware/io_disk)
PROV_FILE = "DEV.CFG"


def list_volumes():
    """列出可移动卷(卷标 + 盘符)。用 PowerShell 的 Get-Volume,最稳。"""
    ps = ("Get-Volume | Where-Object { $_.DriveType -eq 'Removable' -or "
          "$_.FileSystemLabel -eq '%s' } | "
          "ForEach-Object { [pscustomobject]@{ letter=$_.DriveLetter; "
          "label=$_.FileSystemLabel; type=$_.DriveType; size=$_.Size } } | "
          "ConvertTo-Json -Compress" % VOLUME_LABEL)
    try:
        out = subprocess.run(["powershell", "-NoProfile", "-Command", ps],
                             capture_output=True, text=True, timeout=20)
    except (OSError, subprocess.SubprocessError) as exc:
        print("列卷失败:%s" % exc, file=sys.stderr)
        return []
    text = (out.stdout or "").strip()
    if not text:
        return []
    try:
        data = json.loads(text)
    except ValueError:
        print("Get-Volume 输出不是 JSON:%r" % text[:200], file=sys.stderr)
        return []
    if isinstance(data, dict):
        data = [data]
    return data


def find_volume(volumes, label=VOLUME_LABEL):
    for v in volumes:
        if (v.get("label") or "").upper() == label.upper() and v.get("letter"):
            return v
    return None


def main():
    ap = argparse.ArgumentParser(description="write DEV.CFG to the Sophon shared volume")
    ap.add_argument("--ssid", help="WiFi SSID")
    ap.add_argument("--pass", dest="password", default=None,
                    help="WiFi password (omit for an open network)")
    ap.add_argument("--name", help="device name (mDNS instance name)")
    ap.add_argument("--log-ips", dest="log_ips",
                    help="comma-separated source IPs allowed to fetch logs over UDP")
    ap.add_argument("--label", default=VOLUME_LABEL, help="volume label to look for")
    ap.add_argument("--file", default=PROV_FILE, help="provisioning file name")
    ap.add_argument("--dry-run", action="store_true", help="print, do not write")
    ap.add_argument("--list-volumes", action="store_true", help="list removable volumes and exit")
    args = ap.parse_args()

    volumes = list_volumes()
    if args.list_volumes:
        for v in volumes:
            print("%s:  label=%s type=%s size=%s" % (
                v.get("letter"), v.get("label"), v.get("type"), v.get("size")))
        if not volumes:
            print("(没找到可移动卷 —— 设备插上了吗?枚举完成了吗?)")
        return 0

    if not args.ssid and not args.log_ips and not args.name:
        print("至少要给 --ssid / --name / --log-ips 之一", file=sys.stderr)
        return 2

    vol = find_volume(volumes, args.label)
    if not vol:
        print("没找到卷标为 %s 的可移动卷。" % args.label, file=sys.stderr)
        print("找到的可移动卷:" % "", file=sys.stderr)
        for v in volumes:
            print("  %s: label=%s type=%s" % (v.get("letter"), v.get("label"), v.get("type")),
                  file=sys.stderr)
        print("设备插上了吗?Windows 枚举完了吗?(插上后等几秒)", file=sys.stderr)
        return 2

    lines = []
    if args.ssid:
        lines.append("ssid=%s" % args.ssid)
    if args.password is not None:
        lines.append("pass=%s" % args.password)
    if args.name:
        lines.append("name=%s" % args.name)
    if args.log_ips is not None:
        lines.append("logips=%s" % args.log_ips)
    content = "\r\n".join(lines) + "\r\n"

    target = "%s:\\%s" % (vol["letter"], args.file)
    print("目标:%s" % target)
    print("内容:")
    for line in lines:
        # 密码别回显
        print("    %s" % (line if not line.startswith("pass=") else "pass=<hidden>"))
    if args.dry_run:
        print("(dry-run,没写)")
        return 0

    try:
        with open(target, "w", encoding="ascii", newline="") as f:
            f.write(content)
    except OSError as exc:
        print("写 %s 失败:%s" % (target, exc), file=sys.stderr)
        return 1

    try:
        size = os.path.getsize(target)
    except OSError:
        size = -1
    print("已写入 %d 字节。" % size)
    print("设备每 5 秒会看一眼盘上的 %s,变了就应用(第一次约 5~10 秒生效)。" % args.file)
    return 0


if __name__ == "__main__":
    sys.exit(main())
