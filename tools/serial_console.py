#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
serial_console.py -- 与设备 UART0 控制台交互(发命令 + 读输出)。

设备的 io_cfg 组件在 UART0 上跑一个极简命令行(首次配网用):
    wifi scan                扫描附近 AP
    wifi set <ssid> [pass]   写入 WiFi 凭据
    psk set <psk>            写设备 PSK
    name set <name>          改设备名
    status                   打印当前配置与状态

用法:
    python tools/serial_console.py COM3 "wifi set MySSID MyPass" 6
    python tools/serial_console.py COM3 "status" 3

注意:打开串口后必须立刻把 DTR/RTS 拉低,否则 CH343 会复位开发板。
"""

import sys
import time

try:
    import serial
except ImportError:
    print("NO_PYSERIAL")
    sys.exit(3)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    port = sys.argv[1]
    command = sys.argv[2] if len(sys.argv) > 2 else ""
    wait = float(sys.argv[3]) if len(sys.argv) > 3 else 5.0

    ser = serial.Serial()
    ser.port = port
    ser.baudrate = 115200
    ser.timeout = 0.2
    ser.dtr = False
    ser.rts = False
    ser.open()
    ser.dtr = False
    ser.rts = False

    # 先把积压的日志读掉,避免和命令回显混在一起
    deadline = time.time() + 0.5
    while time.time() < deadline:
        try:
            if not ser.read(4096):
                break
        except Exception:
            break

    if command:
        ser.write((command + "\r\n").encode("ascii", "replace"))
        ser.flush()

    out = []
    deadline = time.time() + wait
    while time.time() < deadline:
        try:
            chunk = ser.read(4096)
            if chunk:
                out.append(chunk.decode("utf-8", "replace"))
        except Exception:
            pass

    ser.close()
    text = "".join(out)
    sys.stdout.write(text)
    if "ioagent>" in text:
        print("\n[console] command acknowledged")
        return 0
    print("\n[console] no prompt seen in %.1fs" % wait)
    return 1


if __name__ == "__main__":
    sys.exit(main())
