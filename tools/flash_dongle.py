#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""flash_dongle.py — 给**全新的空白 ESP32-S3 dongle** 装第一个固件。

为什么单独要一个脚本
--------------------
新 dongle 是**单 Type-C** 的:那一个口插在目标机上,没有第二个口给开发机。
所以:

  · **第一次**装固件必须走 **USB** —— 而且只能在这块板子**还没刷过任何东西**
    的时候做。ESP32-S3 出厂时 ROM 里跑的是 **USB-Serial-JTAG**,
    插上电脑就枚举成一个 COM 口 ⇒ esptool 直接能烧。
  · 烧完之后,这个口就变成我们的 **HID+MSC 复合设备**(键盘/鼠标/共享盘),
    **不再是 COM 口** ⇒ 之后的固件更新只能走 **WiFi OTA**
    (这就是为什么 WiFi OTA 是单 Type-C dongle 的必需品)。

★ WiFi OTA 装不了"第一个固件" ★
  没有固件 ⇒ 没有 OTA 客户端 ⇒ 没有 WiFi ⇒ 没有 mDNS ⇒ 找不到设备。
  第一个固件**必须**用 USB。

产线电脑上**不需要装 ESP-IDF**:只要有 esptool(一个 pip 包)和这四个 bin。

用法
----
    python tools/flash_dongle.py                 # 自动找 USB-Serial-JTAG 的 COM 口
    python tools/flash_dongle.py --port COM7
    python tools/flash_dongle.py --build firmware/build
    python tools/flash_dongle.py --list          # 只列出能烧的 COM 口

烧完的预期
----------
    ① COM 口消失(USB-Serial-JTAG 退场)
    ② 出现 HID+MSC 复合设备:目标机上多出"键盘""鼠标""可移动磁盘(SOPHON)"
"""

import argparse
import glob
import os
import subprocess
import sys
import time

# ESP32-S3 出厂/ROM 下载模式的 USB 标识
ESP_USB_JTAG_VID = 0x303A
ESP_USB_JTAG_PID = 0x1001

# 烧录地址(和 firmware/partitions.csv 一致)
FLASH_MAP = [
    (0x0, "bootloader/bootloader.bin"),
    (0x8000, "partition_table/partition-table.bin"),
    (0x10000, "ota_data_initial.bin"),
    (0x20000, "sophon.bin"),
]


def list_serial_ports():
    """列出串口,尽量带上 VID/PID(用来认出 USB-Serial-JTAG)。"""
    try:
        import serial.tools.list_ports as lp
    except ImportError:
        return []
    out = []
    for p in lp.comports():
        out.append({"device": p.device, "vid": p.vid, "pid": p.pid,
                    "desc": p.description or ""})
    return out


def find_jtag_port(explicit=None):
    ports = list_serial_ports()
    if explicit:
        return explicit, ports
    for p in ports:
        if p["vid"] == ESP_USB_JTAG_VID and p["pid"] == ESP_USB_JTAG_PID:
            return p["device"], ports
    return None, ports


def collect_images(build_dir):
    imgs = []
    for addr, rel in FLASH_MAP:
        path = os.path.join(build_dir, rel)
        if not os.path.isfile(path):
            print("缺文件:%s" % path, file=sys.stderr)
            return None
        imgs.append((addr, path))
    return imgs


def esptool_cmd():
    """优先用 esptool 的独立安装,其次用 IDF 自带的。"""
    for exe in ("esptool", "esptool.py"):
        try:
            subprocess.run([exe, "--version"], capture_output=True, timeout=20)
            return [exe]
        except (OSError, subprocess.SubprocessError):
            continue
    return [sys.executable, "-m", "esptool"]


def main():
    ap = argparse.ArgumentParser(description="flash a blank ESP32-S3 dongle over USB")
    ap.add_argument("--port", help="serial port (default: auto-detect USB-Serial-JTAG)")
    ap.add_argument("--build", default=os.path.join(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "firmware", "build"),
        help="directory holding the built images")
    ap.add_argument("--chip", default="esp32s3")
    ap.add_argument("--baud", default="460800")
    ap.add_argument("--list", action="store_true", help="list candidate ports and exit")
    ap.add_argument("--dry-run", action="store_true", help="print the command only")
    args = ap.parse_args()

    port, ports = find_jtag_port(args.port)
    if args.list or not port:
        print("串口一览:")
        for p in ports:
            mark = "  <= 可烧(USB-Serial-JTAG)" if (
                p["vid"] == ESP_USB_JTAG_VID and p["pid"] == ESP_USB_JTAG_PID) else ""
            print("  %-8s vid=%04x pid=%04x  %s%s" % (
                p["device"], p["vid"] or 0, p["pid"] or 0, p["desc"], mark))
        if not port:
            print("", file=sys.stderr)
            print("没找到 USB-Serial-JTAG(VID %04x PID %04x)的串口。" % (
                ESP_USB_JTAG_VID, ESP_USB_JTAG_PID), file=sys.stderr)
            print("  · 新板子插上后要等几秒才枚举", file=sys.stderr)
            print("  · 烧过固件的板子**不再是 COM 口**(变成 HID+MSC)⇒ 那种板子只能走 WiFi OTA",
                  file=sys.stderr)
            return 2
        if args.list:
            return 0

    imgs = collect_images(args.build)
    if not imgs:
        return 2

    cmd = esptool_cmd() + [
        "--chip", args.chip,
        "--port", port,
        "--baud", args.baud,
        "--before", "default_reset",
        "--after", "hard_reset",
        "write_flash",
    ]
    for addr, path in imgs:
        cmd += ["0x%x" % addr, path]

    print("端口:%s" % port)
    for addr, path in imgs:
        print("  0x%06x  %s (%d B)" % (addr, os.path.basename(path),
                                      os.path.getsize(path)))
    if args.dry_run:
        print("(dry-run)")
        print(" ".join(cmd))
        return 0

    print("开始烧录 …", flush=True)
    try:
        rc = subprocess.run(cmd).returncode
    except (OSError, subprocess.SubprocessError) as exc:
        print("调 esptool 失败:%s" % exc, file=sys.stderr)
        return 1
    if rc != 0:
        print("esptool 退出码 %d —— 没烧成功,板子还是原样。" % rc, file=sys.stderr)
        print("常见原因:端口不是 USB-Serial-JTAG / 被别的程序占着 / 按住 BOOT 再插。",
              file=sys.stderr)
        return rc

    print("")
    print("烧录成功。板子会重启并重新枚举:")
    print("  · 那个 COM 口会消失(USB-Serial-JTAG 退场)")
    print("  · 出现 HID+MSC 复合设备:键盘 + 鼠标 + 可移动磁盘(卷标 SOPHON)")
    print("接下来在同一台电脑上配网:")
    print("  python tools/provision.py --ssid <SSID> --pass <PASS> --name <设备名>")
    print("然后拔下来插到目标机,再用 mDNS 发现它。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
