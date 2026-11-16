#!/usr/bin/env python3
"""Check the WorkBuddy connector package against the official submit checklist.

两种接入方案都支持(MCP + Skill / CLI + Skill),按 connector-meta.json 的
`type` 自动选检查集:

  MCP 方案(type 省略或 "mcp")  需要 mcp.json
  CLI 方案(type == "cli")       需要 cli.json

检查项(依据 open.workbuddy.cn/docs/connector,2026-11 核对):
  C1  包结构          connector-meta / mcp.json|cli.json / icon / skills/**/SKILL.md
  C2  source 合规     全局唯一 kebab-case(仅小写字母、数字、连字符)
  C3  auth_mode       合法值;token 模式必须有 token-schema.json
  C4  token 最低版本  auth_mode=token 需 >= 4.23.0
  C5  占位符对应      mcp.json 的 ${VAR} 必须都在 token-schema 的 fields[].key 里
  C6  无真实凭证      不得硬编码 token / key / password
  C7  MCP 配置形态    仅一个 server;type 合法;runtime 仅 node(官方唯一支持)
  C8  SKILL.md 合规   frontmatter 有 name + descriptions
  C9  元信息双语      name/name_zh/name_en + description 三件套
  C10 名字呼应        SKILL 里提到的名字与 server 名一致(软检查)
  C11 作者署名        author 已填(平台详情页展示"谁做的")
  C12 版本一致性      用 4.24.0 字段(name_zh/examples_zh 等)必须声明 >= 4.24.0
  C13 示例字段名      示例是 examples_zh/examples_en,不是裸 examples
  C14 远程地址        url 型 transport 必须 https(生产环境硬要求)
  C15 CLI 配置        CLI 方案:init/unAuth/status 三平台齐全

Usage:
    python wb-connector/validate.py
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

    meta_p = os.path.join(root, "connector-meta.json")
    mcp_p = os.path.join(root, "mcp.json")
    cli_p = os.path.join(root, "cli.json")
    tok_p = os.path.join(root, "token-schema.json")
    icon = next((os.path.join(root, n) for n in ("icon.svg", "icon.png", "icon.jpg")
                 if os.path.exists(os.path.join(root, n))), None)
    skills_dir = os.path.join(root, "skills")
    skill_files = []
    if os.path.isdir(skills_dir):
        for d in sorted(os.listdir(skills_dir)):
            p = os.path.join(skills_dir, d, "SKILL.md")
            if os.path.exists(p):
                skill_files.append(p)

    if not os.path.exists(meta_p):
        print("  [FAIL] C1a  connector-meta.json 缺失")
        return 1
    meta = load_json(meta_p)
    scheme = "cli" if str(meta.get("type", "")).lower() == "cli" else "mcp"
    conf_p = cli_p if scheme == "cli" else mcp_p
    conf_name = os.path.basename(conf_p)

    print("scheme: %s  (config: %s)\n" % (scheme, conf_name))

    # ---- C1 结构
    check("C1a", "connector-meta.json", True)
    check("C1b", conf_name, os.path.exists(conf_p))
    check("C1c", "icon.svg / icon.png / icon.jpg", icon is not None,
          os.path.basename(icon) if icon else "missing")
    check("C1d", "skills/*/SKILL.md", len(skill_files) > 0,
          "%d skill(s)" % len(skill_files))
    # 选了 MCP 就不该同时留 cli.json(一个连接器只能一种方案)
    other = cli_p if scheme == "mcp" else mcp_p
    check("C1e", "另一种方案的配置文件不存在",
          not os.path.exists(other),
          os.path.basename(other) if os.path.exists(other) else "ok")

    if not os.path.exists(conf_p):
        return 1
    conf = load_json(conf_p)

    # ---- C2 source
    src = meta.get("source", "")
    check("C2", "source 是 kebab-case",
          bool(re.fullmatch(r"[a-z0-9]+(-[a-z0-9]+)*", src)), src or "(missing)")

    # ---- C9 双语元信息
    check("C9a", "name + name_zh + name_en",
          all(meta.get(k) for k in ("name", "name_zh", "name_en")))
    check("C9b", "description 三件套",
          all(meta.get(k) for k in ("description", "description_zh", "description_en")))

    # ---- C11 作者
    check("C11", "author 已填", bool(meta.get("author")),
          meta.get("author") or "(missing)")

    # ---- C3 auth_mode(仅 MCP)
    mode = meta.get("auth_mode")
    if scheme == "mcp":
        check("C3a", "auth_mode 合法", mode in (None, "token", "server-side"),
              str(mode))
        if mode == "token":
            check("C3b", "token 模式有 token-schema.json", os.path.exists(tok_p))
    else:
        check("C3a", "CLI 方案不应声明 auth_mode", mode is None, str(mode))

    # ---- 版本相关
    ver = str(meta.get("minWorkbuddyVersion", ""))
    nn = tuple(int(x) for x in re.findall(r"\d+", ver)[:2] or [0, 0])
    if mode == "token":
        check("C4", "token 模式 minWorkbuddyVersion >= 4.23.0", nn >= (4, 23), ver)
    uses_424 = any(meta.get(k) for k in
                   ("name_zh", "name_en", "examples_zh", "examples_en"))
    if uses_424:
        check("C12", "用了 4.24.0 字段 -> 声明 >= 4.24.0", nn >= (4, 24), ver)
    if "examples" in meta:
        check("C13", "示例应为 examples_zh/examples_en", False, "发现裸 examples")

    if scheme == "mcp":
        # ---- C5 占位符
        fields = set()
        if os.path.exists(tok_p):
            for f in load_json(tok_p).get("fields", []):
                if f.get("key"):
                    fields.add(f["key"])
        placeholders = set(re.findall(r"\$\{([A-Za-z0-9_]+)\}",
                                      json.dumps(conf)))
        check("C5a", "${VAR} 都在 token-schema 里", not (placeholders - fields),
              sorted(placeholders - fields) or "%d placeholder(s)" % len(placeholders))
        check("C5b", "token-schema 字段都被用到", not (fields - placeholders),
              sorted(fields - placeholders) or "ok")

        # ---- C7 MCP 配置形态
        servers = conf.get("mcpServers", {})
        check("C7a", "只有一个 MCP server", len(servers) == 1, "%d" % len(servers))
        ok_type, notes = True, []
        for sname, cfg in servers.items():
            t = cfg.get("type", "stdio")
            if t not in ("stdio", "sse", "streamableHttp", "streamable-http", "http"):
                ok_type = False
                notes.append("%s: bad type %s" % (sname, t))
            rt = cfg.get("runtime")
            if rt is not None:
                rtype = rt.get("type")
                # 官方原文:runtime.type 目前仅支持 "node"
                if rtype != "node":
                    ok_type = False
                    notes.append("%s: runtime.type=%s (only node supported)" % (sname, rtype))
            if t != "stdio":
                url = str(cfg.get("url", ""))
                if not url.startswith("https://"):
                    notes.append("%s: url 非 https" % sname)
        check("C7b", "transport / runtime 合法(runtime 仅 node)", ok_type,
              "; ".join(notes) or "ok")

        # ---- C14 远程地址必须 https
        remote = [c for c in servers.values()
                  if c.get("type", "stdio") != "stdio"]
        if remote:
            bad = [c.get("url", "") for c in remote
                   if not str(c.get("url", "")).startswith("https://")]
            check("C14", "远程 url 都是 https", not bad, bad or "ok")

        server_name = next(iter(servers), "")
    else:
        # ---- C15 CLI 配置
        platforms = ("darwin", "linux", "win32")
        for key, need in (("init", True), ("unAuth", True), ("status", True)):
            v = conf.get(key)
            if need:
                ok = isinstance(v, dict) and all(v.get(p) for p in platforms)
                missing = [] if ok else [p for p in platforms
                                         if not (isinstance(v, dict) and v.get(p))]
                check("C15" + key[0], "cli.json %s 三平台齐全" % key, ok,
                      missing or "ok")
        # CLI 侧 runtime 支持 node 与 python(官方表:Node v4.22.0 / Python v5.0.0)
        rt = conf.get("runtime")
        if rt is not None:
            rtype = rt.get("type")
            check("C16", "cli.json runtime.type 合法(node/python)",
                  rtype in ("node", "python"), str(rtype))
            if rtype == "python":
                check("C17", "cli.json python runtime 需 >= 5.0.0",
                      nn >= (5, 0), ver)
        server_name = meta.get("source", "")

    # ---- C6 无硬编码凭证
    suspects = []
    for name in sorted(os.listdir(root)):
        p = os.path.join(root, name)
        if not os.path.isfile(p) or os.path.splitext(name)[1] not in (
                ".json", ".md", ".js", ".py", ".txt"):
            continue
        text = open(p, "r", encoding="utf-8", errors="replace").read()
        for m in re.finditer(
                r'"(?:password|passwd|psk|token|secret|api_?key|ak)"\s*:\s*"([^"]{8,})"',
                text, re.I):
            v = m.group(1)
            if "${" not in v and not v.isalpha():
                suspects.append("%s:%s" % (name, v[:10]))
    check("C6", "无硬编码凭证", not suspects, ", ".join(suspects) or "clean")

    # ---- C8 SKILL frontmatter
    if skill_files:
        head = open(skill_files[0], "r", encoding="utf-8").read()[:3000]
        fm = re.match(r"^---\n(.*?)\n---", head, re.S)
        body = fm.group(1) if fm else ""
        check("C8a", "SKILL.md 有 frontmatter", bool(fm))
        check("C8b", "frontmatter 有 name", "name:" in body)
        check("C8c", "frontmatter 有 descriptions", "descriptions:" in body)

    # ---- C10 名字呼应(软检查)
    if skill_files and server_name:
        text = open(skill_files[0], "r", encoding="utf-8").read().lower()
        check("C10", "SKILL 与配置名呼应", server_name.split("-")[0].lower() in text,
              server_name)

    total, passed = len(results), sum(results)
    print("\n=== summary ===\n  %d checks, %d passed, %d failed"
          % (total, passed, total - passed))
    return 0 if passed == total else 1


if __name__ == "__main__":
    sys.exit(main())
