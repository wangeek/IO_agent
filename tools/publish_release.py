#!/usr/bin/env python3
"""把 sophon-bridge 的 tarball 发到 Release 托管,并匿名回验下载。

为什么需要这个
    WorkBuddy 连接器的 MCP 靠 `npx -y <tarball URL>` 拉起桥,所以 tarball 要有
    一个稳定、公开、可取到的 URL。而 connector 包里没有位置放可执行文件
    (官方包结构只有 connector-meta.json / mcp.json / icon / skills),
    也没打算发 public npm。实测(2026-11-16)npx 能直接跑 http(s) 上的 tarball。

两条路的差别(2026-11-17 核实)
    github  GitHub Release。附件走 uploads.github.com 裸 body 上传。
            GitHub 仓库镜像到 Gitee 之后,**镜像只同步代码/分支/tag,
            不同步 Release 和附件** —— Gitee 那边的发行版必须单独建一次。
    gitee   Gitee Release。附件走 multipart,`POST /releases` 必须带
            `target_commitish`,否则报 target_commitish is missing。
            镜像仓库的代码只读,但可以建 Release(Release 是平台对象,不是 git 对象;
            参照 dromara/Jpom:镜像仓 + 手动建 Release)。

为什么用 curl 而不是 urllib
    本机 Python 连 api.github.com 握手就断(`SSL: UNEXPECTED_EOF_WHILE_READING`),
    而 Windows 自带的 curl 加 `--ssl-no-revoke` 正常(不加会报
    `CRYPT_E_REVOCATION_OFFLINE`:连不上吊销列表服务器)。所以 HTTP 全部走 curl。
    凭据走 `curl -K <临时配置文件>`,不进命令行,免得在进程列表里露出来。

token 从哪来(都不进仓库、不进固件)
    github  GITHUB_TOKEN / GH_TOKEN 环境变量 → --token-file → 本机 git 凭据管理器
            (git credential fill,就是推送时用的那份 OAuth 凭据)
    gitee   GITEE_TOKEN 环境变量 → --token-file
            (gitee.com → 设置 → 私人令牌,权限勾 projects)

用法
    python tools/publish_release.py --dry-run                 # 只查状态,不写任何东西
    python tools/publish_release.py                           # 默认 github,发 dist 里最新 tarball
    python tools/publish_release.py --host gitee              # Gitee 那条(需令牌)
    python tools/publish_release.py --notes-file docs/release-notes-sophon-bridge-0.2.2.md

退出码
    0 成功且匿名回验通过(大小与 sha256 都一致);非 0 说明某一步没成,
    不要宣称发好了。
"""

import argparse
import glob
import hashlib
import json
import mimetypes
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GH_API = "https://api.github.com"
GH_UPLOADS = "https://uploads.github.com"
GITEE_API = "https://gitee.com/api/v5"
TIMEOUT = 60

# 组件前缀 + 语义版本,同仓多组件(tarball / 固件 / 连接器)各打各的 tag 不撞号
TAG_PREFIX = "sophon-bridge-v"

CURL = "curl.exe"


def die(msg):
    print("FAIL: %s" % msg)
    raise SystemExit(1)


class HttpFail(Exception):
    def __init__(self, code, detail, where):
        self.code = code
        self.detail = detail
        Exception.__init__(self, "%s -> HTTP %d %s" % (where, code, detail))


# ---------------------------------------------------------------- 通用小工具

def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()


def parse_version(filename):
    """sophon-bridge-0.2.2.tgz -> (0, 2, 2);排不出数字就当 (0,),不抛。"""
    stem = os.path.basename(filename)
    for ext in (".tgz", ".tar.gz"):
        if stem.endswith(ext):
            stem = stem[: -len(ext)]
            break
    tail = stem.split("-")[-1]
    nums = tuple(int(x) for x in tail.split(".") if x.isdigit())
    return nums or (0,)


def pick_latest_tarball():
    cands = glob.glob(os.path.join(ROOT, "dist", "sophon-bridge-*.tgz"))
    if not cands:
        die("dist 里没有 sophon-bridge-*.tgz,先跑 python tools/pack_bridge.py")
    return max(cands, key=parse_version)


def _cfg_esc(s):
    """curl 配置文件里双引号字符串的转义。"""
    return s.replace("\\", "\\\\").replace('"', '\\"')


def _curl(cfg_lines, timeout=TIMEOUT):
    """跑一次 curl,返回 (http_code, body_bytes)。cfg_lines 是 -K 配置行。"""
    cfg_path = out_path = None
    try:
        fd, cfg_path = tempfile.mkstemp(suffix=".curlcfg")
        with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as f:
            f.write("\n".join(cfg_lines) + "\n")
        fd2, out_path = tempfile.mkstemp(suffix=".curlout")
        os.close(fd2)

        cmd = [CURL, "-sS", "--ssl-no-revoke", "--max-time", str(int(timeout)),
               "-K", cfg_path, "-o", out_path, "-w", "%{http_code}"]
        try:
            p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout + 30)
        except FileNotFoundError:
            die("找不到 %s。这个脚本的 HTTP 层依赖 Windows 自带 curl。" % CURL)
        except subprocess.TimeoutExpired:
            die("curl 超时(%ds): %s" % (timeout, cfg_lines[0][:120]))

        code = (p.stdout or "").strip()
        body = b""
        if os.path.exists(out_path):
            with open(out_path, "rb") as f:
                body = f.read()
        if not code:
            die("curl 没给出状态码: %s" % (p.stderr or "").strip()[:300])
        return code, body
    finally:
        for f in (cfg_path, out_path):
            if f and os.path.exists(f):
                try:
                    os.remove(f)
                except OSError:
                    pass


def http(url, method="GET", headers=None, json_body=None, raw_file=None,
         form=None, timeout=TIMEOUT, ok=(200,)):
    """返回解析后的 JSON(空 body 返回 None)。状态码不在 ok 里就抛 HttpFail。"""
    lines = ['url = "%s"' % _cfg_esc(url)]
    if method != "GET":
        lines.append('request = "%s"' % method)
    for k, v in (headers or {}).items():
        lines.append('header = "%s: %s"' % (_cfg_esc(k), _cfg_esc(v)))

    tmp_body = None
    if json_body is not None:
        fd, tmp_body = tempfile.mkstemp(suffix=".json")
        with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as f:
            json.dump(json_body, f, ensure_ascii=False)
        lines.append('data-binary = "@%s"' % tmp_body.replace("\\", "/"))
    elif raw_file:
        lines.append('data-binary = "@%s"' % os.path.abspath(raw_file).replace("\\", "/"))
    for one in (form or []):
        lines.append('form = "%s"' % _cfg_esc(one))

    try:
        code, body = _curl(lines, timeout=timeout)
    finally:
        if tmp_body and os.path.exists(tmp_body):
            try:
                os.remove(tmp_body)
            except OSError:
                pass

    text = body.decode("utf-8", "replace")
    if str(code) not in [str(c) for c in ok]:
        raise HttpFail(int(code) if str(code).isdigit() else -1,
                       text[:400].replace("\n", " "),
                       "%s %s" % (method, url.split("?")[0]))
    if not text.strip():
        return None
    try:
        return json.loads(text)
    except ValueError:
        raise HttpFail(int(code), "返回的不是 JSON: %s" % text[:200],
                       "%s %s" % (method, url.split("?")[0]))


def verify_anonymous(url, want_sha, want_size):
    """不带任何凭据按最终下载 URL 拉一遍 —— 用户侧就是这么拿的。"""
    fd, out_path = tempfile.mkstemp(suffix=".dl")
    os.close(fd)
    try:
        # 必须跟 -o 到已知路径,要按字节算 sha256;-L 因为 Release 下载会 302 到 CDN
        lines = [
            'url = "%s"' % _cfg_esc(url),
            'header = "User-Agent: sophon-release-check"',
        ]
        cfg_path = out_path + ".cfg"
        with open(cfg_path, "w", encoding="utf-8", newline="\n") as f:
            f.write("\n".join(lines) + "\n")
        try:
            p = subprocess.run(
                [CURL, "-sS", "--ssl-no-revoke", "--max-time", "180", "--location",
                 "-K", cfg_path, "-o", out_path, "-w", "%{http_code}"],
                capture_output=True, text=True, timeout=240)
        finally:
            try:
                os.remove(cfg_path)
            except OSError:
                pass
        hcode = (p.stdout or "").strip()
        if hcode not in ("200", "206"):
            return False, "匿名下载 -> HTTP %s %s" % (hcode or "?", (p.stderr or "").strip()[:160])
        got_size = os.path.getsize(out_path)
        if got_size != want_size:
            return False, "匿名下载大小不符: %d != %d" % (got_size, want_size)
        got = sha256_file(out_path)
        if got != want_sha:
            return False, "匿名下载 sha256 不符: %s" % got
        return True, "HTTP %s, %d bytes, sha256 一致" % (hcode, got_size)
    finally:
        if os.path.exists(out_path):
            try:
                os.remove(out_path)
            except OSError:
                pass


# ---------------------------------------------------------------- token

def token_from_git_credential(host="github.com"):
    """从本机凭据管理器取(推送用的那份 OAuth 凭据)。不打印、不落盘。"""
    payload = ("protocol=https\nhost=%s\n\n" % host).encode()
    try:
        p = subprocess.run([ "git", "credential", "fill"], input=payload,
                           capture_output=True, timeout=60)
    except (OSError, subprocess.SubprocessError) as e:
        return None, "git credential fill 跑不起来: %s" % e
    if p.returncode != 0:
        return None, "凭据管理器没有 %s 的凭据" % host
    for line in p.stdout.decode("utf-8", "replace").splitlines():
        if line.startswith("password="):
            tok = line[len("password="):].strip()
            if tok:
                return tok, None
    return None, "凭据管理器返回里没有 password 字段"


def get_token(host, args):
    for name in (["GITHUB_TOKEN", "GH_TOKEN"] if host == "github" else ["GITEE_TOKEN"]):
        tok = os.environ.get(name)
        if tok:
            return tok, "%s 环境变量" % name
    if args.token_file:
        try:
            with open(args.token_file, encoding="utf-8") as f:
                tok = f.read().strip()
        except OSError as e:
            die("读不了 --token-file: %s" % e)
        if not tok:
            die("--token-file 是空的: %s" % args.token_file)
        return tok, "--token-file"
    if host == "github":
        tok, err = token_from_git_credential()
        if tok:
            return tok, "本机 git 凭据管理器"
        die("没拿到 GitHub 令牌。设 GITHUB_TOKEN,或用 --token-file。(%s)" % err)
    die("没拿到 Gitee 令牌。设 GITEE_TOKEN,或用 --token-file。"
        "(gitee.com → 设置 → 私人令牌,权限勾 projects)")


# ---------------------------------------------------------------- GitHub

def gh_headers(token):
    h = {
        "Accept": "application/vnd.github+json",
        "X-GitHub-Api-Version": "2022-11-28",
        "User-Agent": "sophon-release",
    }
    if token:
        h["Authorization"] = "Bearer " + token
    return h


def gh_find_release(repo, tag, token):
    try:
        return http("%s/repos/%s/releases/tags/%s" % (GH_API, repo, tag),
                    headers=gh_headers(token))
    except HttpFail as e:
        if e.code == 404:
            return None
        raise


def publish_github(repo, tag, title, body, branch, token, source, path,
                   filename, size, sha, dry):
    who = http("%s/user" % GH_API, headers=gh_headers(token))
    print("token   : ok, login=%s (%s)" % (who.get("login"), source))
    info = http("%s/repos/%s" % (GH_API, repo), headers=gh_headers(token))
    print("repo    : %s (private=%s, default_branch=%s)"
          % (info.get("full_name"), info.get("private"), info.get("default_branch")))
    if info.get("private"):
        print("WARN: 仓库私有,npx 匿名拉 tarball 会失败。上架用的仓库必须公开。")

    rel = gh_find_release(repo, tag, token)
    if dry:
        assets = [a.get("name") for a in (rel or {}).get("assets", [])]
        print("release : %s" % ("已存在 id=%s, assets=%s" % (rel["id"], assets)
                                if rel else "不存在(未创建)"))
        print("\n--dry-run,什么都没写。")
        return None

    if rel:
        print("release : 已存在 id=%s,复用" % rel["id"])
    else:
        rel = http("%s/repos/%s/releases" % (GH_API, repo), method="POST",
                   json_body={
                       "tag_name": tag,
                       "target_commitish": branch,
                       "name": title,
                       "body": body,
                       "draft": False,
                       "prerelease": False,
                   },
                   headers=dict(gh_headers(token), **{"Content-Type": "application/json"}),
                   ok=(201,))
        print("release : 已创建 id=%s" % rel["id"])
        print("page    : %s" % rel.get("html_url"))

    have = [a.get("name") for a in (rel.get("assets") or [])]
    if filename in have:
        print("attach  : %s 已在该发行版上,跳过上传" % filename)
    else:
        ctype = mimetypes.guess_type(filename)[0] or "application/octet-stream"
        http("%s/repos/%s/releases/%d/assets?name=%s" % (
                GH_UPLOADS, repo, rel["id"], filename),
             method="POST", raw_file=path,
             headers=dict(gh_headers(token), **{"Content-Type": ctype}),
             timeout=300, ok=(201,))
        print("attach  : 已上传 %s" % filename)

    rel2 = gh_find_release(repo, tag, None) or {}
    asset = next((a for a in (rel2.get("assets") or []) if a.get("name") == filename), None)
    if not asset:
        die("匿名视角看不到附件 %s(发行版可能是 draft,或附件没上传成功)" % filename)
    url = asset.get("browser_download_url")
    if not url:
        die("附件里没有 browser_download_url,字段=%s" % list(asset.keys()))

    # GitHub 自己算的摘要:上传完整与否用它判,不依赖本机网络
    digest = (asset.get("digest") or "").strip()
    digest_ok = bool(digest) and digest.lower() == "sha256:" + sha.lower()
    if digest:
        if digest_ok:
            print("digest  : 与本地一致 (%s…)" % digest[:19])
        else:
            die("GitHub 报的 digest 与本地不符: %s != sha256:%s" % (digest, sha))

    ok, detail = verify_anonymous(url, sha, size)
    print("verify  : %s" % detail)
    if not ok and digest_ok:
        print("WARN    : 上传完整性已由 GitHub digest 确认,失败的是「本机能不能下载」这一步。")
        print("WARN    : github.com 在国内经常 10s+ 才连上、偶发中途超时,不代表发行版坏了。")
        return url, False
    if not ok:
        die("匿名回验没过。")
    return url, True


# ---------------------------------------------------------------- Gitee

def publish_gitee(repo, tag, title, body, branch, token, source, path,
                  filename, size, sha, dry):
    auth = {"Authorization": "token " + token}
    who = http("%s/user" % GITEE_API, headers=auth)
    print("token   : ok, login=%s (%s)" % (who.get("login"), source))
    info = http("%s/repos/%s" % (GITEE_API, repo), headers=auth)
    print("repo    : %s (private=%s, default_branch=%s)"
          % (info.get("full_name"), info.get("private"), info.get("default_branch")))
    if info.get("private"):
        print("WARN: 仓库私有,npx 匿名拉 tarball 会失败。上架用的仓库必须公开。")

    rels = http("%s/repos/%s/releases?per_page=100" % (GITEE_API, repo), headers=auth) or []
    rel = next((r for r in rels if r.get("tag_name") == tag), None)
    if dry:
        print("release : %s" % ("已存在 id=%s" % rel["id"] if rel else "不存在(未创建)"))
        print("\n--dry-run,什么都没写。")
        return None

    if rel:
        print("release : 已存在 id=%s,复用" % rel["id"])
    else:
        # 表单而不是 JSON:target_commitish 必填,漏了报 target_commitish is missing
        rel = http("%s/repos/%s/releases" % (GITEE_API, repo), method="POST",
                   form=[
                       "access_token=%s" % token,
                       "tag_name=%s" % tag,
                       "name=%s" % title,
                       "body=%s" % body,
                       "target_commitish=%s" % branch,
                       "prerelease=false",
                   ],
                   headers={"Content-Type": "application/x-www-form-urlencoded"},
                   ok=(201, 200))
        print("release : 已创建 id=%s" % rel["id"])

    have = [a.get("name") for a in (rel.get("assets") or [])]
    if filename in have:
        print("attach  : %s 已在该发行版上,跳过上传" % filename)
    else:
        http("%s/repos/%s/releases/%d/attach_files" % (GITEE_API, repo, rel["id"]),
             method="POST",
             form=["access_token=%s" % token,
                   "file=@%s" % os.path.abspath(path).replace("\\", "/")],
             timeout=300, ok=(200, 201))
        print("attach  : 已上传 %s" % filename)

    rels2 = http("%s/repos/%s/releases?per_page=100" % (GITEE_API, repo)) or []
    rel2 = next((r for r in rels2 if r.get("tag_name") == tag), None)
    if not rel2:
        die("匿名视角看不到 tag %s 的发行版(仓库可能私有)" % tag)
    asset = next((a for a in (rel2.get("assets") or []) if a.get("name") == filename), None)
    if not asset:
        die("匿名视角看不到附件 %s" % filename)
    url = asset.get("browser_download_url")
    if not url:
        die("附件里没有 browser_download_url,字段=%s" % list(asset.keys()))
    ok, detail = verify_anonymous(url, sha, size)
    print("verify  : %s" % detail)
    if not ok:
        print("WARN    : Gitee 附件接口不返回摘要,没法像 GitHub 那样单独证明上传完整。")
        return url, False
    return url, True


# ---------------------------------------------------------------- 事后补验

def verify_existing(host, repo, tag, filename, sha, size):
    """只验已发布的发行版:匿名读元数据 + 匿名下载核对。返回 (url, verified)。"""
    digest = ""
    if host == "github":
        rel = gh_find_release(repo, tag, None) or {}
        asset = next((a for a in (rel.get("assets") or []) if a.get("name") == filename), None)
        if not asset:
            die("匿名视角找不到 %s 的发行版 %s" % (tag, filename))
        url = asset.get("browser_download_url")
        digest = (asset.get("digest") or "").strip()
        if digest:
            if digest.lower() == "sha256:" + sha.lower():
                print("digest  : 与本地一致 (%s…)" % digest[:19])
            else:
                die("GitHub 报的 digest 与本地不符: %s != sha256:%s" % (digest, sha))
    else:
        rels = http("%s/repos/%s/releases?per_page=100" % (GITEE_API, repo)) or []
        rel = next((r for r in rels if r.get("tag_name") == tag), None)
        if not rel:
            die("匿名视角找不到 tag %s 的发行版" % tag)
        asset = next((a for a in (rel.get("assets") or []) if a.get("name") == filename), None)
        if not asset:
            die("匿名视角找不到附件 %s" % filename)
        url = asset.get("browser_download_url")

    if not url:
        die("附件里没有 browser_download_url")

    ok, detail = verify_anonymous(url, sha, size)
    print("verify  : %s" % detail)
    if ok:
        return url, True
    if digest and digest.lower() == "sha256:" + sha.lower():
        print("WARN    : digest 一致(上传完整),失败的是本机下载。")
    return url, False


# ---------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", choices=["github", "gitee"], default="github")
    ap.add_argument("--repo", default="wangeek/IO_agent", help="owner/name")
    ap.add_argument("--tag", help="默认 %s<版本>,如 %s0.2.2" % (TAG_PREFIX, TAG_PREFIX))
    ap.add_argument("--file", help="tarball,默认 dist 下版本号最大的 sophon-bridge-*.tgz")
    ap.add_argument("--name", help="发行版标题,默认与 tag 相同")
    ap.add_argument("--branch", default="main", help="target_commitish")
    ap.add_argument("--notes", default="", help="发行版说明正文")
    ap.add_argument("--notes-file", help="从文件读说明正文(推荐,免去引号转义)")
    ap.add_argument("--token")
    ap.add_argument("--token-file")
    ap.add_argument("--dry-run", action="store_true", help="只查状态,不建发行版、不上传")
    ap.add_argument("--verify-only", action="store_true",
                    help="只对已发布的发行版重跑匿名下载回验(网络差时事后补验用)")
    args = ap.parse_args()

    path = args.file or pick_latest_tarball()
    if not os.path.isabs(path):
        path = os.path.join(ROOT, path)
    if not os.path.exists(path):
        die("找不到文件: %s" % path)
    filename = os.path.basename(path)
    size = os.path.getsize(path)
    sha = sha256_file(path)

    tag = args.tag or (TAG_PREFIX + ".".join(str(x) for x in parse_version(filename)))
    title = args.name or tag

    body = args.notes
    if args.notes_file:
        nf = args.notes_file
        if not os.path.isabs(nf):
            nf = os.path.join(ROOT, nf)
        with open(nf, encoding="utf-8") as f:
            body = f.read().strip()
    if not body:
        body = "sophon-bridge %s" % ".".join(str(x) for x in parse_version(filename))

    print("host    : %s" % args.host)
    print("repo    : %s" % args.repo)
    print("tag     : %s" % tag)
    print("file    : %s" % path)
    print("size    : %d bytes" % size)
    print("sha256  : %s" % sha)

    if args.verify_only:
        url, verified = verify_existing(args.host, args.repo, tag, filename, sha, size)
        if not url:
            return 1
        print("\nurl     : %s" % url)
        if verified:
            print("结论    : 匿名可下载,大小与 sha256 都对。")
            return 0
        print("结论    : 本机没能匿名下载回来核验(多半是网络);发行版本身可能没问题。")
        return 2

    if args.token:
        token, source = args.token, "--token"
    else:
        token, source = get_token(args.host, args)

    pub = publish_github if args.host == "github" else publish_gitee
    try:
        url, verified = pub(args.repo, tag, title, body, args.branch, token, source,
                            path, filename, size, sha, args.dry_run)
    except HttpFail as e:
        die(str(e))

    if args.dry_run or not url:
        return 0

    print("\n--- 填进 wb-connector/mcp.json 的 args ---")
    print(url)
    print("\n--- 给 npx 的自检命令 ---")
    print('npx -y "%s" --version' % url)
    if not verified:
        print("\n结论    : 发行版已发布,但本机没能匿名下载回验。换网络后跑:")
        print("          python tools/publish_release.py --host %s --tag %s --verify-only"
              % (args.host, tag))
        return 2
    print("\n结论    : 已发布且匿名可下载,大小与 sha256 都对。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
