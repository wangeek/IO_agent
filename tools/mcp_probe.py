#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
mcp_probe.py -- 对设备做裸 HTTP 构造访问(不依赖任何 MCP 客户端库)。

分两层验证:
  L1  HTTP 层:GET /ping —— 设备活着吗?能应答吗?
  L2  JSON-RPC 层:POST /mcp —— initialize / tools/list / tools/call

用法:
    python tools/mcp_probe.py 172.17.21.225
    python tools/mcp_probe.py 172.17.21.225 device/info
    python tools/mcp_probe.py 172.17.21.225 observe '{"target":"preview"}'

注意:
  * PowerShell 里直接 curl.exe -d '{...}' 会把 JSON 的双引号吃掉(实测报
    "invalid JSON"),所以这里用 urllib 从 Python 发,或用 --data-binary @file。
  * 默认只跑**只读**工具。act 是会往目标机注入按键的动作,必须显式点名。
"""

import json
import sys
import urllib.error
import urllib.request

TIMEOUT = 12


def http_get(url):
    req = urllib.request.Request(url, method="GET")
    with urllib.request.urlopen(req, timeout=TIMEOUT) as r:
        return r.status, r.read().decode("utf-8", "replace")


def rpc(url, method, params=None, rpc_id=1):
    body = {"jsonrpc": "2.0", "id": rpc_id, "method": method}
    if params is not None:
        body["params"] = params
    data = json.dumps(body).encode("utf-8")
    req = urllib.request.Request(url, data=data, method="POST",
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=TIMEOUT) as r:
        return r.status, r.read().decode("utf-8", "replace")


def call_tool(url, name, args=None):
    status, raw = rpc(url, "tools/call", {"name": name, "arguments": args or {}})
    obj = json.loads(raw)
    result = obj.get("result", {})
    if "content" in result and result["content"]:
        text = result["content"][0].get("text", "")
        try:
            return json.loads(text)
        except Exception:
            return text
    return obj


def deploy_file(url, local_path, slot=0):
    """把本地文件通过远程 deploy_payload 下发到设备的 flash 仓。

    这条路径**不碰目标机的任何东西**:内容只进设备 flash,
    下次开机由 overlay 铺到 RAM disk 槽位。
    """
    with open(local_path, "rb") as f:
        raw = f.read()
    import base64
    payload = base64.b64encode(raw).decode("ascii")
    print("deploying %s (%d bytes -> %d b64) to slot %d" %
          (local_path, len(raw), len(payload), slot))
    return call_tool(url, "deploy_payload", {"slot": slot, "base64": payload})


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    ip = sys.argv[1]
    base = "http://%s:8080" % ip
    url = base + "/mcp"

    if len(sys.argv) >= 3:
        name = sys.argv[2]
        if name == "deploy":
            if len(sys.argv) < 4:
                print("need: deploy <localfile> [slot]")
                return 2
            slot = int(sys.argv[4]) if len(sys.argv) > 4 else 0
            print(json.dumps(deploy_file(url, sys.argv[3], slot),
                             ensure_ascii=False, indent=2))
            return 0
        args = {}
        if len(sys.argv) > 3:
            raw = sys.argv[3]
            if raw.lstrip().startswith("{"):
                args = json.loads(raw)
            else:
                # 简写:直接给一个字符串参数(避免 PowerShell 传 JSON 时
                # 把双引号剥掉 —— 那个坑踩过两次)
                args = {"target": raw}
        print(json.dumps(call_tool(url, name, args), ensure_ascii=False, indent=2))
        return 0

    print("=== L1: GET /ping ===")
    try:
        st, body = http_get(base + "/ping")
        print("HTTP %d  %s" % (st, body))
    except Exception as e:
        print("FAIL: %s" % e)
        return 1

    print("")
    print("=== L2a: initialize ===")
    try:
        st, body = rpc(url, "initialize", {})
        print("HTTP %d  %s" % (st, body))
    except Exception as e:
        print("FAIL: %s" % e)
        return 1

    print("")
    print("=== L2b: tools/list ===")
    try:
        st, body = rpc(url, "tools/list")
        obj = json.loads(body)
        tools = obj.get("result", {}).get("tools", [])
        print("HTTP %d  %d tool(s)" % (st, len(tools)))
        for t in tools:
            print("  - %s" % t.get("name"))
    except Exception as e:
        print("FAIL: %s" % e)
        return 1

    print("")
    for tool, args in (("device/info", None),
                       ("watcher/status", None),
                       ("observe", {"target": "preview"})):
        print("=== L2c: tools/call %s ===" % tool)
        try:
            print(json.dumps(call_tool(url, tool, args), ensure_ascii=False, indent=2))
        except Exception as e:
            print("FAIL: %s" % e)
        print("")
    return 0


if __name__ == "__main__":
    sys.exit(main())
