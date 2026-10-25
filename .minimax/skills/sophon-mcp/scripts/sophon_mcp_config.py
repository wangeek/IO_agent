#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""sophon_mcp_config.py — 让 MCP 配置里的 URL 永远指向设备当前的 IP。

问题
----
Sophon 跑的是 streamable-http 的 MCP server(设备是 server,agent 连它),
所以**客户端配置里必须写设备的 URL**。而设备的 IP 是 **DHCP 分配的** ——
换网段、租约到期、换板子都会变。IP 一变,配置就指向一个死地址,
表现是"这个 MCP server 连不上"。

为什么不能直接写 `http://sophon-lab.local:8080/mcp`:
Windows 的**系统解析器解析不了 `.local`**(实测 Resolve-DnsName 报
"DNS name does not exist")。设备侧的 mDNS 公告是好的,但消费它需要
**我们自己的解析器** —— `discover.py` 就是干这个的。

用法
----
    python tools/sophon_mcp_config.py              # 发现 + 核对 + 必要时更新
    python tools/sophon_mcp_config.py --check      # 只核对,不改
    python tools/sophon_mcp_config.py --config <path>
    python tools/sophon_mcp_config.py --name sophon-lab

它做三件事:
  1. 用 mDNS 找到设备当前的地址(discover.py);
  2. 读 MCP 配置文件里的 url,和实际地址比对;
  3. 不一致就改写,然后**明确告诉你需要重连**。

为什么要做成脚本而不是让大模型自己拼 URL 改 json:
  拼错一个字段就得到一个"配好了但连不上"的配置,还很难看出错在哪。
  一条命令 = 一步到位,大模型只需要跑它、看它的输出。

★ 改完之后**当前连接不会自动切换** ★
  MCP server 的 url 是**客户端启动时读一次**的。改了配置,新的 URL 从
  **下一次连接**起生效(重启 MCP 连接 / 新开会话)。
  这个脚本会把这句话一起打出来,免得 agent 以为改完就好了。
"""

import argparse
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
# discover.py 与本脚本同目录(skill 是自包含的,要能独立分发)
DISCOVER = os.path.join(HERE, "discover.py")
# 默认只管**当前工作区**的 .mcp.json(agent 的 cwd 就是工作区)。
# 全局 ~/.minimax/mcp.json 是用户环境、跨项目共用,且按约定全局条目一律
# enabled:false —— **不该由这个脚本去动**。真要动就显式 --config 指过去。
DEFAULT_CONFIG = os.path.join(os.getcwd(), ".mcp.json")
DEFAULT_SERVER = "sophon"


def discover(name, service, timeout):
    if not os.path.isfile(DISCOVER):
        print("找不到 discover.py:%s" % DISCOVER, file=sys.stderr)
        return None
    cmd = [sys.executable, DISCOVER, "--name", name, "--service", service,
           "--json", "--timeout", str(timeout)]
    try:
        out = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout + 8)
    except (OSError, subprocess.SubprocessError) as exc:
        print("discover 失败:%s" % exc, file=sys.stderr)
        return None
    try:
        data = json.loads(out.stdout)
    except ValueError:
        print("discover 输出不是 JSON:%r" % out.stdout[:200], file=sys.stderr)
        return None
    if not data.get("found"):
        return None
    return data


def load_config(path):
    if not os.path.isfile(path):
        return None
    try:
        with open(path, "r", encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError) as exc:
        print("读配置失败:%s(%s)" % (path, exc), file=sys.stderr)
        return None


def main():
    ap = argparse.ArgumentParser(description="keep the Sophon MCP url pointing at the device")
    ap.add_argument("--name", default="sophon-lab", help="device name (NVS 'name')")
    ap.add_argument("--service", default="_ioagent._tcp.local", help="mDNS service type")
    ap.add_argument("--config", default=DEFAULT_CONFIG,
                    help="MCP config file (default: this workspace's .mcp.json)")
    ap.add_argument("--server", default=DEFAULT_SERVER, help="server entry name in the config")
    ap.add_argument("--timeout", type=float, default=4.0, help="mDNS listen seconds")
    ap.add_argument("--check", action="store_true", help="only report, do not write")
    args = ap.parse_args()

    found = discover(args.name, args.service, args.timeout)
    if not found:
        print("没找到设备 %s(mDNS %s)。" % (args.name, args.service), file=sys.stderr)
        print("检查:设备通电?在同一个网段?mDNS 公告起来了?"
              " —— 设备起来后 mDNS 要几秒。", file=sys.stderr)
        return 2

    url = found.get("url")
    print("设备当前地址: %s  (host=%s, ip=%s)" % (url, found.get("host"), found.get("address")))

    cfg = load_config(args.config)
    if cfg is None:
        print("配置读不到:%s" % args.config, file=sys.stderr)
        if os.path.abspath(args.config) == os.path.abspath(DEFAULT_CONFIG):
            print("(当前工作区还没有 .mcp.json —— 这个项目要用 sophon MCP 就得有一份;"
                  " 可参考仓库根的 .mcp.json 模板)", file=sys.stderr)
        return 2
    servers = cfg.get("mcpServers") or {}
    entry = servers.get(args.server)
    if not isinstance(entry, dict):
        print("配置里没有 '%s' 条目(%s)。" % (args.server, args.config), file=sys.stderr)
        return 2

    current = entry.get("url")
    if current == url:
        print("配置已经指向它了: %s" % url)
        return 0

    if args.check:
        print("不一致:配置=%s 实际=%s(没改,--check)" % (current, url))
        return 1

    entry["url"] = url
    tmp = args.config + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(cfg, f, indent=2, ensure_ascii=False)
        f.write("\n")
    os.replace(tmp, args.config)
    print("已更新 %s 里 '%s' 的 url:" % (args.config, args.server))
    print("    %s" % current)
    print(" -> %s" % url)
    print("★ 当前 MCP 连接不会自动切换(url 是客户端启动时读一次的)。")
    print("  重启 MCP 连接 / 新开会话后生效。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
