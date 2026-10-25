#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""uart_capture.py -- 抓一段 UART0 日志(排查 panic / 启动问题)。

用法:
    python tools/uart_capture.py --seconds 20 --out boot.log
"""

import argparse
import sys
import time

try:
    import serial
except ImportError:
    print("pyserial not installed", file=sys.stderr)
    sys.exit(2)


def main():
    ap = argparse.ArgumentParser(description="Capture Sophon UART0 output")
    ap.add_argument("--port", default="COM3")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--seconds", type=float, default=20.0)
    ap.add_argument("--out", help="write the capture to this file")
    ap.add_argument("--reset", action="store_true",
                    help="pulse DTR/RTS to reset the board before capturing")
    args = ap.parse_args()

    ser = serial.Serial(args.port, args.baud, timeout=0.2)
    if args.reset:
        # CH343 的 DTR/RTS 接在 ESP32 的 EN/BOOT 上:拉低 RTS 再放开 = 复位
        ser.dtr = False
        ser.rts = True
        time.sleep(0.15)
        ser.rts = False
        time.sleep(0.2)

    out = open(args.out, "w", encoding="utf-8", errors="replace") if args.out else None
    end = time.time() + args.seconds
    buf = ""
    while time.time() < end:
        chunk = ser.read(4096)
        if chunk:
            text = chunk.decode("utf-8", "replace")
            buf += text
            if out:
                out.write(text)
                out.flush()
            else:
                sys.stdout.write(text)
                sys.stdout.flush()
    ser.close()
    if out:
        out.close()
        print("wrote %d chars -> %s" % (len(buf), args.out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
