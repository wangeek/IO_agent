#!/usr/bin/env python3
"""把 wb-connector/ 打成 WorkBuddy 审核要的 zip。

官方要求(2026-11 核对,两处独立来源一致):
  · **只收 zip**;
  · **zip 根层直接是文件**(connector-meta.json / mcp.json / icon.svg / skills/…),
    **不能再套一层目录** —— 套了平台解析不到。
    (反例:"不能套一层目录"这条是实机提交过的第三方包踩出来的)

不打包的东西:
  README.md      给人和审核看的工程说明,不是运行时资源
  validate.py    自查脚本,平台不需要
  __pycache__/   Python 缓存
  任何 .pyc / .local / 临时文件

用法:
    python tools/pack_connector.py                 # → dist/sophon-connector-<ver>.zip
    python tools/pack_connector.py --list          # 只列出会打进去的文件
"""

import argparse
import json
import os
import sys
import zipfile

SRC = r"D:\Lab\IO_agent\wb-connector"
OUT_DIR = r"D:\Lab\IO_agent\dist"

# 提交必需(缺一个就打不动)
REQUIRED = ["connector-meta.json", "mcp.json", "icon.svg"]

# 明确排除
EXCLUDE_NAMES = {"README.md", "validate.py"}
EXCLUDE_DIRS = {"__pycache__", ".git", ".claude"}


def collect(src):
    """收集要打包的文件(相对路径,正斜杠)。"""
    out = []
    for root, dirs, files in os.walk(src):
        dirs[:] = [d for d in dirs if d not in EXCLUDE_DIRS]
        for f in sorted(files):
            if f in EXCLUDE_NAMES or f.endswith((".pyc", ".pyo", ".zip")):
                continue
            full = os.path.join(root, f)
            rel = os.path.relpath(full, src).replace("\\", "/")
            out.append((rel, full))
    return sorted(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--list", action="store_true", help="只列出文件,不打包")
    args = ap.parse_args()

    if not os.path.isdir(SRC):
        print("missing source dir: %s" % SRC)
        return 2

    # 先自检元信息(免得打出一个审核必拒的包)
    meta_p = os.path.join(SRC, "connector-meta.json")
    if not os.path.exists(meta_p):
        print("missing connector-meta.json")
        return 2
    with open(meta_p, encoding="utf-8") as f:
        meta = json.load(f)
    version = str(meta.get("version", "0.0.0"))
    source = str(meta.get("source", "connector"))

    missing = [n for n in REQUIRED if not os.path.exists(os.path.join(SRC, n))]
    if missing:
        print("missing required files: %s" % ", ".join(missing))
        return 2

    files = collect(SRC)
    total = sum(os.path.getsize(p) for _, p in files)

    print("source : %s" % SRC)
    print("version: %s   source id: %s" % (version, source))
    print("files  : %d  (%.1f KB)" % (len(files), total / 1024.0))
    for rel, full in files:
        print("   %8d  %s" % (os.path.getsize(full), rel))

    if args.list:
        return 0

    os.makedirs(OUT_DIR, exist_ok=True)
    zip_path = os.path.join(OUT_DIR, "%s-connector-%s.zip" % (source, version))
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as z:
        for rel, full in files:
            # 关键:arcname 用相对路径,保证根层就是文件(不套目录)
            z.write(full, rel)

    # 回读校验:确认根层确有必需文件、且没有多余顶层目录
    with zipfile.ZipFile(zip_path) as z:
        names = z.namelist()
        roots = sorted({n.split("/")[0] for n in names})
        has_flagship = all(r in roots for r in REQUIRED)
        dirs_at_root = [r for r in roots
                        if any(n.startswith(r + "/") for n in names)
                        and r not in ("skills",)]

    print("\nwrote %s (%.1f KB)" % (zip_path, os.path.getsize(zip_path) / 1024.0))
    print("root entries: %s" % ", ".join(roots))
    if not has_flagship:
        print("WARN: required files are not at zip root")
        return 1
    if dirs_at_root:
        print("WARN: unexpected top-level dirs: %s" % dirs_at_root)
        return 1
    print("OK: 三个必需文件都在根层,没有多余的顶层目录")
    return 0


if __name__ == "__main__":
    sys.exit(main())
