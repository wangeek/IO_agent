#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""diag_big_response.py -- 定位"大响应间歇性失败"是服务端还是客户端。

现象(2026-10-25):contract_probe 间歇性地把 tools/list / status / read /
GET /files/<name> 判失败(HTTP 状态 0 = 连接层错误),而小响应(错误信封、
404)永远成功。设备侧日志同时出现 `httpd_sock_err: error in send : 11`
(EAGAIN = socket 发送缓冲满)。

这个脚本对同一个端点分别用两种方式各打 N 次:
  A. urllib(探针用的方式)
  B. 裸 socket(自己读 header + Content-Length)
对比失败率,就能分清是"urllib 读法的问题"还是"服务端发不动"。

用法:
    python tools/diag_big_response.py 172.17.21.225
    python tools/diag_big_response.py 172.17.21.225 --n 30
"""

import argparse
import json
import socket
import sys
import time
import urllib.request

TIMEOUT = 10.0


def via_urllib(ip, port, path, body=None, close=False):
    url = "http://%s:%d%s" % (ip, port, path)
    data = body.encode("utf-8") if body else None
    req = urllib.request.Request(url, data=data, method="POST" if data else "GET")
    if data:
        req.add_header("Content-Type", "application/json")
    if close:
        # http.client 默认 keep-alive,服务端会把连接留在池里;
        # 池满后 esp_http_server 的 LRU purge 会掐掉"正在传大响应"的那个。
        req.add_header("Connection", "close")
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    t0 = time.time()
    try:
        with opener.open(req, timeout=TIMEOUT) as r:
            payload = r.read()
            return r.status, len(payload), time.time() - t0, ""
    except Exception as exc:                      # noqa: BLE001
        return 0, 0, time.time() - t0, "%s: %s" % (type(exc).__name__, exc)


def via_socket(ip, port, path, body=None):
    if body:
        raw = ("POST %s HTTP/1.1\r\nHost: %s\r\nContent-Type: application/json\r\n"
               "Content-Length: %d\r\nConnection: close\r\n\r\n%s"
               % (path, ip, len(body), body)).encode("utf-8")
    else:
        raw = ("GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n"
               % (path, ip)).encode("ascii")
    t0 = time.time()
    try:
        s = socket.create_connection((ip, port), timeout=TIMEOUT)
    except OSError as exc:
        return 0, 0, time.time() - t0, "connect: %s" % exc
    try:
        s.sendall(raw)
        buf = b""
        while b"\r\n\r\n" not in buf:
            chunk = s.recv(4096)
            if not chunk:
                break
            buf += chunk
        if b"\r\n\r\n" not in buf:
            return 0, 0, time.time() - t0, "no headers: %r" % buf[:120]
        head, _, rest = buf.partition(b"\r\n\r\n")
        lines = head.decode("utf-8", "replace").split("\r\n")
        status = 0
        hdrs = {}
        for i, line in enumerate(lines):
            if i == 0:
                try:
                    status = int(line.split()[1])
                except (IndexError, ValueError):
                    status = 0
            elif ":" in line:
                k, _, v = line.partition(":")
                hdrs[k.strip().lower()] = v.strip()
        try:
            clen = int(hdrs.get("content-length", "0") or 0)
        except ValueError:
            clen = 0
        body_buf = rest
        while len(body_buf) < clen:
            chunk = s.recv(4096)
            if not chunk:
                break
            body_buf += chunk
        if len(body_buf) < clen:
            return status, len(body_buf), time.time() - t0, \
                "short body: %d/%d" % (len(body_buf), clen)
        return status, len(body_buf), time.time() - t0, ""
    except OSError as exc:
        return 0, 0, time.time() - t0, "io: %s" % exc
    finally:
        s.close()


def run(label, fn, ip, port, path, body, n, **kw):
    fails = []
    sizes = []
    times = []
    for i in range(n):
        st, size, dt, err = fn(ip, port, path, body, **kw)
        times.append(dt)
        if st != 200 or err:
            fails.append((i, st, size, err))
        else:
            sizes.append(size)
    avg = sum(times) / len(times) if times else 0
    print("%-22s %d/%d ok, avg %.0f ms, body~%s" % (
        label, n - len(fails), n, avg * 1000,
        (str(sum(sizes) // len(sizes)) + "B") if sizes else "-"))
    for f in fails:
        print("    FAIL #%d status=%s size=%s err=%s" % f)
    return len(fails)


def main():
    ap = argparse.ArgumentParser(description="Diagnose intermittent large-response failures")
    ap.add_argument("ip")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--n", type=int, default=20)
    args = ap.parse_args()

    rpc = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "tools/call",
                      "params": {"name": "status", "arguments": {}}})

    print("=== target %s:%d, %d rounds each ===" % (args.ip, args.port, args.n))
    total = 0
    total += run("urllib keepalive", via_urllib, args.ip, args.port, "/mcp", rpc, args.n)
    total += run("urllib +Conn:close", via_urllib, args.ip, args.port, "/mcp", rpc,
                 args.n, close=True)
    total += run("socket  +Conn:close", via_socket, args.ip, args.port, "/mcp", rpc, args.n)
    print("=== total failures: %d ===" % total)
    return 0 if total == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
