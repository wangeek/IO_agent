#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""read_fetch_check.py -- 读文件超时之后,东西到底在不在?

判据:如果超时之后 `GET /files/<name>` 能拿到字节,说明**处理函数跑完了**,
问题在响应路径(应答丢了);如果拿不到,说明处理函数本身卡住了。

用法: python tools/read_fetch_check.py 172.17.21.225 [FILE]
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from io_mcp_client import McpClient  # noqa: E402

HOST = sys.argv[1] if len(sys.argv) > 1 else "172.17.21.225"
NAME = sys.argv[2] if len(sys.argv) > 2 else "TEST.TXT"

c = McpClient(HOST, timeout=20)

t0 = time.time()
try:
    res = c.call("read", {"name": NAME})
    print("read  HTTP ok  %.2fs  %s" % (time.time() - t0, res))
except Exception as exc:  # noqa: BLE001
    print("read  FAILED after %.2fs: %s %s" % (
        time.time() - t0, type(exc).__name__, str(exc)[:70]))

print("--- GET /files/%s ---" % NAME)
t1 = time.time()
try:
    st, data = c.get("/files/%s" % NAME)
    print("files HTTP %d  %.2fs  bytes=%d  %r" % (st, time.time() - t1, len(data), data[:40]))
except Exception as exc:  # noqa: BLE001
    print("files FAILED after %.2fs: %s %s" % (
        time.time() - t1, type(exc).__name__, str(exc)[:70]))
