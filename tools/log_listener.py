#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""log_listener.py -- 收 Sophon dongle 通过 WiFi 发来的日志。

设备端把日志同时送 UART 和 **UDP**(见 firmware/io_log/)。
单 Type-C 口的 dongle 没有第二个 USB 口 ⇒ 串口这条路会消失,日志只能从这儿看。

形态:**开发机主动发一个 UDP 包过去,设备把日志回过来**(一问一答)。
为什么不是"设备广播过来":
  ① **Windows 防火墙**。开发机的网络位置通常是 Public,入站默认拦。
     设备主动发包是 unsolicited 入站 ⇒ 每台开发机都要管理员加放行规则
     (实测本机就卡在这)。而这里开发机**先发**,设备的回包走同一条会话
     (回程流量),Windows 的状态化防火墙放行 ⇒ **一条防火墙规则都不用加**。
  ② 广播可能被 AP 丢掉(IGMP snooping / 客户端隔离),而且丢了设备侧看不出来。
  ③ 拉模型连晚了还能补上:设备侧有 8 KB 的日志尾巴,每次要日志都先补发再转实时。

**授权**:设备端有一张预先配置好的源 IP 授权表(DEV.CFG 的 `logips=`)。
不在表里的源地址,设备的请求**直接丢,连回包都不给**。
所以这个客户端能不能拿到日志,取决于**你把自己的 IP 写进了那张表**。

用法:
    python tools/log_listener.py                          # 连 172.17.21.225
    python tools/log_listener.py --host 172.17.21.225
    python tools/log_listener.py --out device.log         # 同时存文件
    python tools/log_listener.py --bind-port 8091         # 本地收包端口
    python tools/log_listener.py --poll 0.2               # 要日志的间隔(秒)

按 Ctrl-C 退出。
"""

import argparse
import os
import socket
import struct
import sys
import time

MAGIC = b"SL"
HDR = struct.Struct("!2sBBHH")     # magic, ver, flags, seq, len
DEFAULT_BIND_PORT = 8091
DEFAULT_POLL = 0.2


def main():
    ap = argparse.ArgumentParser(description="Tail Sophon device logs over UDP")
    ap.add_argument("--host", default="172.17.21.225", help="device IP")
    ap.add_argument("--port", type=int, default=8090, help="device log port")
    ap.add_argument("--bind-port", type=int, default=DEFAULT_BIND_PORT,
                    help="local port to receive on (0 = let the OS pick)")
    ap.add_argument("--poll", type=float, default=DEFAULT_POLL,
                    help="seconds between log requests (default %.1f)" % DEFAULT_POLL)
    ap.add_argument("--out", help="also write the log stream to this file")
    ap.add_argument("--timeout", type=float, default=5.0, help="socket timeout")
    args = ap.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.bind(("", args.bind_port))
    except OSError as exc:
        if args.bind_port:
            print("bind %d failed (%s) -- falling back to an ephemeral port" % (
                args.bind_port, exc), file=sys.stderr)
            sock.bind(("", 0))
        else:
            raise
    sock.settimeout(args.timeout)

    target = (args.host, args.port)
    print("polling udp://%s:%d  (local port %d, every %.2fs)"
          % (args.host, args.port, sock.getsockname()[1], args.poll), flush=True)
    print("if nothing arrives, the device does not have your IP in its allowlist "
          "(DEV.CFG: logips=<your ip>)", flush=True)
    if args.out:
        print("writing to %s" % args.out, flush=True)
        out = open(args.out, "a", encoding="utf-8", errors="replace")
    else:
        out = None

    last_seq = None
    lost = 0
    try:
        while True:
            sock.sendto(b"LOG\n", target)
            try:
                data, _ = sock.recvfrom(4096)
            except socket.timeout:
                continue

            if len(data) < HDR.size:
                continue
            magic, ver, flags, seq, length = HDR.unpack(data[:HDR.size])
            if magic != MAGIC:
                continue
            payload = data[HDR.size:HDR.size + length]
            if last_seq is not None and seq != (last_seq + 1) & 0xFFFF:
                lost += 1
            last_seq = seq

            text = payload.decode("utf-8", "replace")
            for line in text.splitlines():
                shown = "%s %s" % (time.strftime("%H:%M:%S"), line)
                print(shown, flush=True)
                if out:
                    out.write(shown + "\n")
            if out:
                out.flush()
    except KeyboardInterrupt:
        if lost:
            print("\n(%d frame(s) lost in transit -- UDP is best-effort)" % lost,
                  file=sys.stderr)
        print("\nbye", flush=True)
    finally:
        sock.close()
        if out:
            out.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
