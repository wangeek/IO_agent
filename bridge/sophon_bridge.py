#!/usr/bin/env python3
"""sophon_bridge — 把 Sophon dongle 的 MCP(HTTP)接成一个 stdio MCP server。

为什么需要这个桥
    dongle 的 MCP 端点是 **HTTP**,而且 IP 是 DHCP 分的。桌面 Agent(WorkBuddy
    等)的 MCP 配置里写的 url 是静态的 —— 设备一换 IP,配置就指向死地址。
    另外远程 HTTP 型连接器在上架时还要求公网可达 / HTTPS,而 dongle 就在
    同一张局域网里。

    所以这里做两件事:
      ① **mDNS 自动发现** —— 每次要用时现找设备,IP 变了也跟得上;
      ② **stdio 传输** —— 让只支持本地进程的客户端也能用。

设计原则:透明代理,不翻译语义
    本脚本**不重新实现**任何工具语义,只把 stdin 来的 JSON-RPC 原样 POST 给
    设备的 /mcp,把回包原样写回 stdout。好处是设备端契约(工具集、schema、
    isError 语义)永远是唯一真源,桥不会随设备演进漂移。
    (哑设备原则的同一思路:只搬运,不解读。)

用法
    python sophon_bridge.py                     # mDNS 发现设备,阻塞读 stdin
    python sophon_bridge.py --name sophon-dongle
    python sophon_bridge.py --endpoint http://172.17.21.231:8080/mcp
    python sophon_bridge.py --psk mysecret      # 也可用环境变量 SOPHON_PSK

环境变量
    SOPHON_PSK      设备 PSK(可选;当前固件尚未校验,先留好接口)
    SOPHON_ENDPOINT 直接指定端点(跳过 mDNS)
    SOPHON_NAME     设备名(默认 sophon-dongle)

退出码
    0  正常结束(stdin EOF)
    2  参数/发现错误
"""

import argparse
import json
import os
import socket
import struct
import sys
import time
import urllib.error
import urllib.request

MDNS_ADDR = "224.0.0.251"
MDNS_PORT = 5353
DEFAULT_SERVICE = "_ioagent._tcp.local"
DEFAULT_NAME = "sophon-dongle"
DEFAULT_PORT = 8080

HTTP_TIMEOUT = 30.0


# ------------------------------------------------------------------ mDNS
# 这一段与 scripts/discover.py 同源(纯标准库)。两个要点:
#   ① 系统解析器解析不了 ".local"(Windows 实测),必须自己说 mDNS;
#   ② 必须 JOIN 组播组 224.0.0.251 —— 只 bind 5353 不等于是成员,
#      AP 开了 IGMP snooping 时入不了组就收不到任何应答。

def _encode_name(name):
    out = bytearray()
    for label in name.rstrip(".").split("."):
        raw = label.encode("ascii")
        out.append(len(raw))
        out += raw
    out.append(0)
    return bytes(out)


def _parse_name(data, offset):
    labels, jumped, end, hops = [], False, offset, 0
    while True:
        length = data[offset]
        if length == 0:
            offset += 1
            if not jumped:
                end = offset
            break
        if length & 0xC0 == 0xC0:
            offset = ((length & 0x3F) << 8) | data[offset + 1]
            if not jumped:
                end = offset
            jumped = True
            hops += 1
            if hops > 8:
                raise ValueError("compression loop")
            continue
        offset += 1
        labels.append(data[offset:offset + length].decode("ascii", "replace"))
        offset += length
        if not jumped:
            end = offset
    return ".".join(labels), end


def _discover(name=DEFAULT_NAME, wait_s=5.0):
    """返回 (ip, port) 或 None。"""
    query = (struct.pack("!HHHHHH", 0, 0, 1, 0, 0, 0)
             + _encode_name(DEFAULT_SERVICE)
             + struct.pack("!HH", 12, 1))       # PTR

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.bind(("", 0))                      # 临时端口:不抢 5353
        sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 1)
        try:
            mreq = struct.pack("4s4s", socket.inet_aton(MDNS_ADDR),
                               socket.inet_aton("0.0.0.0"))
            sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq)
        except OSError:
            pass                                # 入不了组也先发查询试试
        sock.settimeout(0.5)
        deadline = time.time() + wait_s
        sent = 0.0

        while time.time() < deadline:
            now = time.time()
            if now - sent > 1.0:                # 每秒重发一次查询
                try:
                    sock.sendto(query, (MDNS_ADDR, MDNS_PORT))
                except OSError:
                    pass
                sent = now
            try:
                data, _ = sock.recvfrom(4096)
            except socket.timeout:
                continue
            found = _parse_mdns(data, name)
            if found:
                return found
        return None
    finally:
        sock.close()


def _parse_mdns(data, want_name):
    """从一份 mDNS 报文里抽 (ip, port)。"""
    if len(data) < 12:
        return None
    _, _, qd, an, _, _ = struct.unpack("!HHHHHH", data[:12])
    offset = 12
    for _ in range(qd):                         # 跳过 question
        try:
            _, offset = _parse_name(data, offset)
        except (ValueError, IndexError):
            return None
        offset += 4

    srv_target, port = None, None
    ip_by_name = {}
    try:
        for _ in range(an):
            owner, offset = _parse_name(data, offset)
            rtype, _, _, rdlength = struct.unpack("!HHIH", data[offset:offset + 10])
            offset += 10
            rdata_off = offset
            offset += rdlength
            if rtype == 33 and rdlength >= 7:                       # SRV
                _, _, port_v = struct.unpack("!HHH", data[rdata_off:rdata_off + 6])
                target, _ = _parse_name(data, rdata_off + 6)
                if want_name.lower() in owner.lower() or True:
                    srv_target, port = target, port_v
            elif rtype == 1 and rdlength == 4:                      # A
                ip_by_name[owner.lower()] = socket.inet_ntoa(
                    data[rdata_off:rdata_off + 4])
    except (ValueError, IndexError, struct.error):
        pass

    if srv_target and srv_target.lower() in ip_by_name:
        return ip_by_name[srv_target.lower()], port or DEFAULT_PORT
    if port and len(ip_by_name) == 1:
        return list(ip_by_name.values())[0], port
    return None


# ------------------------------------------------------------------ HTTP

class Device:
    """设备端点:记住上次解析结果,失败后重新发现一次。"""

    # 发现失败后的冷却期:避免"设备没插"时每条调用都白等一次 mDNS 扫描
    # (一次扫描默认 5 秒,连调三个工具就是 15 秒)。
    FAIL_COOLDOWN_S = 10.0

    def __init__(self, endpoint=None, name=DEFAULT_NAME, psk=None):
        self.endpoint = endpoint
        self.name = name
        self.psk = psk
        self._resolved = None
        self._failed_at = 0.0

    def resolve(self, force=False):
        if self._resolved and not force:
            return self._resolved
        if self.endpoint:
            self._resolved = self.endpoint
            return self._resolved

        now = time.time()
        if not force and self._failed_at and (now - self._failed_at) < self.FAIL_COOLDOWN_S:
            raise RuntimeError(
                "no Sophon device named %r found over mDNS "
                "(retrying in a few seconds; is the dongle powered and on the "
                "same network?)" % self.name)

        found = _discover(self.name)
        if not found:
            self._failed_at = now
            raise RuntimeError(
                "no Sophon device named %r found over mDNS "
                "(is the dongle powered and on the same network?)" % self.name)
        ip, port = found
        self._resolved = "http://%s:%d/mcp" % (ip, port)
        self._failed_at = 0.0
        return self._resolved

    def post(self, payload):
        """把 JSON-RPC 请求体打给设备,返回**原始响应文本**(可能为空)。"""
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        for attempt in (1, 2):
            url = self.resolve(force=(attempt == 2))
            req = urllib.request.Request(url, data=body, method="POST")
            req.add_header("Content-Type", "application/json")
            # MCP Streamable HTTP 要求客户端声明两种都接受
            req.add_header("Accept", "application/json, text/event-stream")
            if self.psk:
                req.add_header("Authorization", "Bearer " + self.psk)
            try:
                with urllib.request.urlopen(req, timeout=HTTP_TIMEOUT) as resp:
                    return resp.read().decode("utf-8", "replace")
            except urllib.error.HTTPError as e:
                # 4xx/5xx 也带着 JSON-RPC 错误体 —— 照原样回给客户端
                try:
                    return e.read().decode("utf-8", "replace")
                except Exception:
                    return ""
            except (urllib.error.URLError, OSError):
                if attempt == 2:
                    raise
                # 第一次失败:IP 可能变了,强制重发现后再试一次
                self._resolved = None


def _unwrap_sse(text):
    """设备若回 text/event-stream,把每条 data: 取出(每行一条 JSON)。"""
    if not text.lstrip().startswith(("event:", "data:")):
        return text
    out = []
    for line in text.splitlines():
        line = line.strip()
        if line.startswith("data:"):
            out.append(line[5:].strip())
    return "\n".join(out)


# ------------------------------------------------------------------ stdio 主循环

def main():
    ap = argparse.ArgumentParser(
        description="Expose a Sophon dongle as a stdio MCP server")
    ap.add_argument("--endpoint", default=os.environ.get("SOPHON_ENDPOINT"),
                    help="MCP endpoint URL (default: mDNS discovery)")
    ap.add_argument("--name", default=os.environ.get("SOPHON_NAME", DEFAULT_NAME),
                    help="device name for mDNS discovery")
    ap.add_argument("--psk", default=os.environ.get("SOPHON_PSK"),
                    help="device PSK, sent as a Bearer token")
    ap.add_argument("--wait", type=float, default=5.0,
                    help="mDNS discovery window in seconds")
    args = ap.parse_args()

    dev = Device(endpoint=args.endpoint, name=args.name, psk=args.psk)

    # 端点解析:成功就先打印,失败**不退出**。
    #
    # 为什么失败也要继续跑:stdio MCP 客户端看到的是"子进程退出" —— 那是一条
    # 无法区分「设备没插」「名字写错」「网络不通」的笼统失败,用户拿不到任何
    # 有用信息。继续跑的话,每条 tools/call 都会带回具体的 JSON-RPC 错误
    # (含 "no Sophon device ... found over mDNS"),模型和用户都看得见,
    # 而且设备一插上,下一次调用就能用(不需要重启连接器)。
    try:
        print("sophon_bridge: device at %s" % dev.resolve(), file=sys.stderr)
    except Exception as e:
        print("sophon_bridge: %s" % e, file=sys.stderr)
        print("sophon_bridge: starting anyway -- each call will report the "
              "failure until the device is reachable", file=sys.stderr)

    stdin = sys.stdin.buffer
    stdout = sys.stdout.buffer

    while True:
        line = stdin.readline()
        if not line:
            break                                   # EOF:客户端关了
        line = line.strip()
        if not line:
            continue
        try:
            payload = json.loads(line.decode("utf-8"))
        except ValueError as e:
            err = {"jsonrpc": "2.0", "id": None,
                   "error": {"code": -32700, "message": "parse error: %s" % e}}
            stdout.write((json.dumps(err) + "\n").encode("utf-8"))
            stdout.flush()
            continue

        try:
            raw = dev.post(payload)
        except Exception as e:
            if "id" not in payload:                 # 通知没有回包义务
                continue
            err = {"jsonrpc": "2.0", "id": payload.get("id"),
                   "error": {"code": -32603,
                             "message": "device unreachable: %s" % e}}
            stdout.write((json.dumps(err) + "\n").encode("utf-8"))
            stdout.flush()
            continue

        text = _unwrap_sse(raw).strip()
        if not text:
            continue                                # 202/通知:无回包
        for chunk in text.splitlines():
            chunk = chunk.strip()
            if chunk:
                stdout.write((chunk + "\n").encode("utf-8"))
        stdout.flush()

    return 0


if __name__ == "__main__":
    sys.exit(main())
