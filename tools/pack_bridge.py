#!/usr/bin/env python3
"""打 sophon-bridge 的 npm tarball —— 给"不发布 public npm"这条路用。

背景
    WorkBuddy 的 MCP + stdio 要靠 `npx` 拉起桥,而 connector 包里**没有位置能
    放可执行文件**(官方包结构只有 connector-meta.json / mcp.json / icon /
    skills)。所以桥必须有地方可取。
    实测(2026-11-16)npx 支持直接跑 **tarball**,本地路径与 http(s) URL 都行:

        npx -y ./sophon-bridge-0.2.0.tgz --version   -> 0.2.0
        npx -y http://host/sophon-bridge-0.2.0.tgz --version -> 0.2.0

    ⇒ 把 tarball 挂到任何静态托管(GitHub Release / 对象存储 / 自己的服务器)
    就能用,不需要 npm 账号、不需要公开上架。

用法
    python tools/pack_bridge.py                 # 只打包
    python tools/pack_bridge.py --url-base https://example.com/sophon
                                                # 同时生成 mcp.json 片段给 --url-base
"""

import argparse
import glob
import hashlib
import json
import os
import shutil
import subprocess
import sys

BRIDGE_DIR = r"D:\Lab\IO_agent\bridge"
OUT_DIR = r"D:\Lab\IO_agent\dist"


def run(cmd, cwd):
    p = subprocess.run(cmd, cwd=cwd, shell=True, capture_output=True, text=True)
    if p.returncode != 0:
        print(p.stdout)
        print(p.stderr)
        raise SystemExit("command failed: %s" % cmd)
    return p.stdout.strip()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--url-base", help="tarball 将挂在这个前缀下(用于生成 mcp.json 片段)")
    args = ap.parse_args()

    pkg_path = os.path.join(BRIDGE_DIR, "package.json")
    with open(pkg_path, encoding="utf-8") as f:
        pkg = json.load(f)
    name, version = pkg["name"], pkg["version"]

    os.makedirs(OUT_DIR, exist_ok=True)

    # 清掉上一次的 tgz(只清本包的,不动别人的)
    for old in glob.glob(os.path.join(BRIDGE_DIR, "%s-*.tgz" % name)):
        os.remove(old)

    out = run("npm pack --silent", cwd=BRIDGE_DIR)
    produced = out.splitlines()[-1].strip()
    src = os.path.join(BRIDGE_DIR, produced)
    if not os.path.exists(src):
        raise SystemExit("npm pack did not produce %s" % produced)

    dst = os.path.join(OUT_DIR, produced)
    shutil.move(src, dst)

    size = os.path.getsize(dst)
    sha = hashlib.sha256(open(dst, "rb").read()).hexdigest()

    print("tarball : %s" % dst)
    print("size    : %d bytes (%.1f KB)" % (size, size / 1024.0))
    print("sha256  : %s" % sha)

    # 自检:这个 tarball 真能被 npx 跑起来吗(本地路径,不联网)
    try:
        v = run('npx -y "./%s" --version' % os.path.basename(dst), cwd=OUT_DIR)
        print("npx 自检: --version -> %s" % v)
        if v.strip() != version:
            print("WARN: 版本号不一致(package.json=%s)" % version)
    except SystemExit as e:
        print("WARN: npx 自检失败: %s" % e)

    if args.url_base:
        base = args.url_base.rstrip("/")
        url = "%s/%s" % (base, os.path.basename(dst))
        snippet = {
            "mcpServers": {
                "sophon": {
                    "type": "stdio",
                    "command": "npx",
                    "args": ["-y", url],
                    "runtime": {"type": "node", "version": ">=18.0.0"},
                    "env": {
                        "SOPHON_NAME": "${SOPHON_NAME}",
                        "SOPHON_PSK": "${SOPHON_PSK}",
                    },
                    "timeout": 30000,
                }
            }
        }
        print("\n--- mcp.json (自托管 tarball 版) ---")
        print(json.dumps(snippet, ensure_ascii=False, indent=2))
        print("\n把这个 URL 写进 wb-connector/mcp.json 的 args 即可。")

    return 0


if __name__ == "__main__":
    sys.exit(main())
