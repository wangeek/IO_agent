#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""read_fetch.py -- 把共享盘上的一个文件拉到本地。

现在设备是"按文件名取"的:
  - `read` 工具直接读盘上的文件(小文件内联,大的给 URL)
  - `GET /files/<name>` 拿**原始字节**(图片就是图片,不是 JSON)

以前这条路是 observe -> blob 槽位 -> /blob/b0,三步;现在文件名就是地址,
一次就够。

用法:
    python tools/read_fetch.py 172.17.21.225 AGENT.SH
    python tools/read_fetch.py 172.17.21.225 SNAP_P.JPG out.jpg
"""

import base64
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from io_mcp_client import McpClient, image_block  # noqa: E402


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    host = sys.argv[1]
    name = sys.argv[2] if len(sys.argv) > 2 else "AGENT.SH"
    out = sys.argv[3] if len(sys.argv) > 3 else None

    c = McpClient(host)

    # 一次 tools/call 就够了:小文件走文本/图片内容块,大的回 URL。
    env = c.call_raw("read", {"name": name})
    if "error" in env:
        print("ERROR: %s" % env["error"])
        return 1

    result = env.get("result", {})
    sc = result.get("structuredContent", {}) or {}

    # 图片:走 MCP 原生 image 块(base64 内联)
    img = image_block(env)
    if img and img.get("data"):
        data = base64.b64decode(img["data"])
        print("%s: %d bytes, %s (inline image block)" % (name, len(data), img.get("mimeType")))
        if out:
            with open(out, "wb") as f:
                f.write(data)
            print("saved -> %s" % out)
        else:
            print("(no output path given; pass one to save the bytes)")
        return 0

    # 文本:直接给
    if sc.get("text") is not None:
        print("%s: %d bytes" % (name, sc.get("bytes", 0)))
        if out:
            with open(out, "wb") as f:
                f.write(sc["text"].encode("utf-8"))
            print("saved -> %s" % out)
        else:
            print(sc["text"])
        return 0

    # 太大:设备只给 URL
    if sc.get("url"):
        print("%s: %d bytes (too large to inline)" % (name, sc.get("size", 0)))
        st, data = c.get("/files/%s" % name)
        print("GET /files/%s -> HTTP %d, %d bytes" % (name, st, len(data)))
        if out and st == 200:
            with open(out, "wb") as f:
                f.write(data)
            print("saved -> %s" % out)
        elif not out:
            print(data[:120])
        return 0 if st == 200 else 1

    print(json.dumps(sc, ensure_ascii=False, indent=2))
    return 0 if sc.get("ok") else 1


if __name__ == "__main__":
    sys.exit(main())
