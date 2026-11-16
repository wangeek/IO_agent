#!/usr/bin/env python3
"""Check the WorkBuddy connector package against the official submit checklist.

Checks (from open.workbuddy.cn/docs/connector):
  C1  包结构完整      connector-meta.json / mcp.json / icon / skills/**/SKILL.md
  C2  source 合规     全局唯一 kebab-case
  C3  auth_mode       合法值,且 token 模式必须有 token-schema.json
  C4  minWorkbuddyVersion  token 模式需 >= 4.23.0
  C5  占位符一一对应  mcp.json 里的 ${VAR} 必须都在 token-schema 的 fields[].key 里
  C6  无真实凭证      包里不能出现长得像 key/token/password 的硬编码值
  C7  MCP 配置形态    仅一个 server;type 合法(stdio/sse/streamableHttp)
  C8  SKILL.md 合规   frontmatter 有 name + descriptions
  C9  元信息双语      name/name_zh/name_en + description 三件套
  C10 工具名对齐      mcp.json 声明的 server 名与 SKILL.md 里讲的一致(软检查)

Usage:
    python wb-connector/validate.py
    python wb-connector/validate.py --dir wb-connector
"""

import argparse
import json
import os
import re
import sys

results = []


def check(tag, desc, ok, detail=""):
    results.append(bool(ok))
    print("  [%s] %-4s %-46s %s"
          % ("PASS" if ok else "FAIL", tag, desc, detail))


def load_json(path):
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default=os.path.dirname(os.path.abspath(__file__)))
    args = ap.parse_args()
    root = args.dir

    print("checking %s\n" % root)

    # ---- C1 结构
    meta_p = os.path.join(root, "connector-meta.json")
    mcp_p = os.path.join(root, "mcp.json")
    tok_p = os.path.join(root, "token-schema.json")
    icon_p = next((os.path.join(root, n) for n in ("icon.svg", "icon.png")
                   if os.path.exists(os.path.join(root, n))), None)
    skills_dir = os.path.join(root, "skills")
    skill_files = []
    if os.path.isdir(skills_dir):
        for d in os.listdir(skills_dir):
            p = os.path.join(skills_dir, d, "SKILL.md")
            if os.path.exists(p):
                skill_files.append(p)

    check("C1a", "connector-meta.json", os.path.exists(meta_p))
    check("C1b", "mcp.json", os.path.exists(mcp_p))
    check("C1c", "icon.svg / icon.png", icon_p is not None,
          os.path.basename(icon_p) if icon_p else "missing")
    check("C1d", "skills/*/SKILL.md", len(skill_files) > 0,
          "%d skill(s)" % len(skill_files))

    if not (os.path.exists(meta_p) and os.path.exists(mcp_p)):
        return 1

    meta = load_json(meta_p)
    mcp = load_json(mcp_p)

    # ---- C2 source
    src = meta.get("source", "")
    check("C2", "source 是 kebab-case", bool(re.fullmatch(r"[a-z0-9]+(-[a-z0-9]+)*", src)),
          src or "(missing)")

    # ---- C9 双语元信息
    trio = all(meta.get(k) for k in ("name_zh", "name_en"))
    check("C9a", "name + name_zh + name_en", bool(meta.get("name")) and trio)
    descs = all(meta.get(k) for k in ("description", "description_zh",
                                      "description_en"))
    check("C9b", "description 三件套", descs)

    # ---- C3 auth_mode
    mode = meta.get("auth_mode")
    check("C3a", "auth_mode 合法", mode in (None, "token", "server-side"),
          str(mode))
    if mode == "token":
        check("C3b", "token 模式有 token-schema.json", os.path.exists(tok_p))

    # ---- C4 版本
    ver = str(meta.get("minWorkbuddyVersion", ""))
    major_minor = tuple(int(x) for x in re.findall(r"\d+", ver)[:2] or [0, 0])
    need = (4, 23) if mode == "token" else (0, 0)
    check("C4", "minWorkbuddyVersion >= 4.23.0 (token 模式)",
          major_minor >= need, ver or "(missing)")

    # ---- C5 占位符
    fields = set()
    if os.path.exists(tok_p):
        schema = load_json(tok_p)
        for f in schema.get("fields", []):
            if f.get("key"):
                fields.add(f["key"])
    blob = json.dumps(mcp)
    placeholders = set(re.findall(r"\$\{([A-Za-z0-9_]+)\}", blob))
    unknown = placeholders - fields
    unused = fields - placeholders
    check("C5a", "${VAR} 都在 token-schema 里", not unknown,
          "unknown=%s" % sorted(unknown) if unknown else "%d placeholder(s)" % len(placeholders))
    check("C5b", "token-schema 字段都被用到", not unused,
          "unused=%s" % sorted(unused) if unused else "ok")

    # ---- C6 无硬编码凭证
    suspects = []
    for name in os.listdir(root):
        p = os.path.join(root, name)
        if not os.path.isfile(p) or os.path.splitext(name)[1] not in (
                ".json", ".md", ".js", ".py", ".txt"):
            continue
        text = open(p, "r", encoding="utf-8", errors="replace").read()
        # 只找"看起来像真的"值:长且不像占位符/单词
        for m in re.finditer(
                r'"(?:password|passwd|psk|token|secret|api_?key|ak)"\s*:\s*"([^"]{8,})"',
                text, re.I):
            v = m.group(1)
            if "${" not in v and not v.isalpha():
                suspects.append("%s:%s" % (name, v[:12]))
    check("C6", "无硬编码凭证", not suspects, ", ".join(suspects) or "clean")

    # ---- C7 MCP 配置形态
    servers = mcp.get("mcpServers", {})
    check("C7a", "只有一个 MCP server", len(servers) == 1,
          "%d" % len(servers))
    ok_type = True
    for _, cfg in servers.items():
        t = cfg.get("type", "stdio")
        if t not in ("stdio", "sse", "streamableHttp", "streamable-http", "http"):
            ok_type = False
    check("C7b", "transport type 合法", ok_type)

    # ---- C8 SKILL frontmatter
    if skill_files:
        head = open(skill_files[0], "r", encoding="utf-8").read()[:2000]
        fm = re.match(r"^---\n(.*?)\n---", head, re.S)
        body = fm.group(1) if fm else ""
        check("C8a", "SKILL.md 有 frontmatter", bool(fm))
        check("C8b", "frontmatter 有 name", "name:" in body)
        check("C8c", "frontmatter 有 descriptions", "descriptions:" in body)

    # ---- C10 server 名与 skill 里提到的名字一致(软检查)
    server_name = next(iter(servers), "")
    if skill_files and server_name:
        text = open(skill_files[0], "r", encoding="utf-8").read()
        check("C10", "SKILL 与 server 名呼应", server_name.split("-")[0].lower()
              in text.lower(), "server=%s" % server_name)

    total, passed = len(results), sum(results)
    print("\n=== summary ===\n  %d checks, %d passed, %d failed"
          % (total, passed, total - passed))
    return 0 if passed == total else 1


if __name__ == "__main__":
    sys.exit(main())
