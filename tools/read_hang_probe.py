#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""read_hang_probe.py -- 回归"读已存在文件会不会把设备读挂"。

历史事故(2026-10-23):observe 读一个**已存在**的文件(21 字节的 AGENT.SH、
宿主现写的 TEST.TXT 都一样)→ 设备 Guru Meditation / LoadProhibited
(EXCVADDR=0x45)→ 重启 → MCP 消失。读一个**不存在**的文件则 0.2s 正常返回。

隔离实验(设备刚复位,一次一个请求):
  1. 宿主往虚拟盘上写一个小文件
  2. read 那个宿主写的文件     -> 是不是"任何已存在文件"都会挂?
  3. read 一个设备槽位文件     -> 是不是只对槽位文件挂?
  4. read 一个不存在的文件     -> 对照组(应该瞬间返回)

用法: python tools/read_hang_probe.py 172.17.21.225
"""

import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from io_mcp_client import McpClient  # noqa: E402

HOST = sys.argv[1] if len(sys.argv) > 1 else "172.17.21.225"
TIMEOUT = 12.0


def find_volume():
    """Find the IOAGENT volume by label (drive letters can change)."""
    if len(sys.argv) > 2:
        return sys.argv[2]
    ps = ("Get-Volume | Where-Object { $_.FileSystemLabel -eq 'IOAGENT' } |"
          " Select-Object -First 1 -ExpandProperty DriveLetter")
    try:
        out = subprocess.run(["powershell", "-NoProfile", "-Command", ps],
                             capture_output=True, text=True, timeout=30).stdout
        for tok in out.split():
            if len(tok) == 1 and tok.isalpha():
                return tok + ":"
    except (OSError, subprocess.SubprocessError):
        pass
    return None


def read_one(c, name):
    """Read one file and report how long it took."""
    t0 = time.time()
    try:
        res = c.call("read", {"name": name})
        dt = time.time() - t0
        return dt, "ok=%s bytes=%s error=%s" % (
            res.get("ok"), res.get("bytes"), res.get("error"))
    except Exception as exc:  # noqa: BLE001
        return time.time() - t0, "%s: %s" % (type(exc).__name__, str(exc)[:90])


def main():
    vol = find_volume()
    print("volume: %s" % vol)
    if vol:
        path = vol + "\\TEST.TXT"
        try:
            with open(path, "wb") as f:
                f.write(b"HELLO_FROM_HOST_12345")
            print("wrote %s (%d bytes)" % (path, os.path.getsize(path)))
        except OSError as exc:
            print("write failed: %s" % exc)

    c = McpClient(HOST, timeout=TIMEOUT)
    for name in ("TEST.TXT", "AGENT.SH", "NOPE.TXT"):
        dt, out = read_one(c, name)
        print("read %-10s %6.2fs :: %s" % (name, dt, out))
        time.sleep(1.0)

    # The device must still be alive after all of that.
    try:
        st = c.rpc("ping")
        print("ping after the reads -> %s" % ("alive" if "result" in st else "DEAD"))
    except Exception as exc:  # noqa: BLE001
        print("ping after the reads -> DEAD (%s)" % exc)


if __name__ == "__main__":
    main()
