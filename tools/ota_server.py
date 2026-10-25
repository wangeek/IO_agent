#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""ota_server.py -- 起一个静态文件服务,让设备能来拉固件。

设备端的 `flash` 工具是 **pull** 模型:设备主动去下载镜像(devtools/ota_server
或 tools/ota_server.py 起的服务),不是开发机往设备里推。

为什么是 pull:
  push(开发机上传 900 KB)会把设备**唯一的 httpd task** 占住整个传输时长
  —— 那个 httpd 是单 task,占住 = MCP 整个服务卡死。pull 走的是设备自己的
  下载任务,边下边写另一个 OTA 槽,不影响 MCP。

用法:
    python tools/ota_server.py                      # 服务 firmware/build,端口 8000
    python tools/ota_server.py --dir firmware/build --port 8000
    python tools/ota_server.py --file D:\\path\\to\\sophon.bin

设备那边:
    mcp_call.py --tool flash --args '{"url":"http://<本机IP>:8000/sophon.bin"}'

刷完后设备会**自己重启**（MCP 连接会断）,重新发现即可。
"""

import argparse
import http.server
import os
import socket
import socketserver
import sys


class Handler(http.server.SimpleHTTPRequestHandler):
    """Static file server with a log line per request."""

    def log_message(self, fmt, *args):
        sys.stderr.write("[ota_server] %s %s\n" % (self.address_string(), fmt % args))
        sys.stderr.flush()

    def end_headers(self):
        # The device streams the whole image; no caching surprises.
        self.send_header("Cache-Control", "no-store")
        super().end_headers()


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


def local_ip():
    """Best guess of the address the device can reach us on."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))          # no packets sent, just picks the route
        return s.getsockname()[0]
    except OSError:
        return "127.0.0.1"
    finally:
        s.close()


def main():
    ap = argparse.ArgumentParser(description="Serve a firmware image for OTA")
    ap.add_argument("--dir", help="directory to serve (default: firmware/build)")
    ap.add_argument("--file", help="serve this single file (overrides --dir)")
    ap.add_argument("--port", type=int, default=8000)
    ap.add_argument("--host", default="0.0.0.0")
    args = ap.parse_args()

    if args.file:
        directory = os.path.dirname(os.path.abspath(args.file))
        name = os.path.basename(args.file)
        os.chdir(directory)
    else:
        repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        directory = os.path.abspath(args.dir or os.path.join(repo, "firmware", "build"))
        os.chdir(directory)
        name = "sophon.bin"

    if not os.path.exists(name):
        print("firmware image not found: %s" % os.path.join(directory, name),
              file=sys.stderr)
        print("build it first:  tools\\idf.ps1 build", file=sys.stderr)
        return 1

    size = os.path.getsize(name)
    ip = local_ip()
    print("serving %s (%d bytes) on http://%s:%d/" % (name, size, ip, args.port))
    print("device command:")
    print('  mcp_call.py --tool flash --args \'{"url":"http://%s:%d/%s"}\'' %
          (ip, args.port, name))
    sys.stdout.flush()

    with Server((args.host, args.port), Handler) as httpd:
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            print("\nbye", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
