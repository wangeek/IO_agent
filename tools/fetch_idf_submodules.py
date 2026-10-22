#!/usr/bin/env python3
"""Fetch ESP-IDF git submodules via GitHub codeload tarballs (proxy-friendly).

Why this exists
---------------
The local HTTP proxy truncates long-lived TLS transfers. That breaks git's
smart-HTTP protocol mid-pack:

    fatal: early EOF
    fatal: fetch-pack: invalid index-pack output
    error: RPC failed; curl 56 schannel: server closed abruptly (missing close_notify)

GitHub's codeload endpoint serves each commit as ONE plain file, which curl can
resume with a Range request, so a truncated transfer is continued instead of
discarded. That makes the fetch survive the flaky proxy.

Integrity
---------
A truncated-but-resumed download could still be silently corrupt. So every
extracted file is verified against GitHub's git-tree API: the locally computed
git blob SHA-1 (``sha1("blob <len>\\0" + content)``) must equal the blob SHA
recorded for that path in the upstream tree. A corrupt byte anywhere fails the
check. Nested gitlinks (submodules inside a submodule) are reported but not
verified, because the codeload tarball does not carry their contents.

Usage
-----
    python tools/fetch_idf_submodules.py --idf-path D:\\Espressif\\frameworks\\esp-idf
    python tools/fetch_idf_submodules.py --idf-path ... --only components/mqtt/esp-mqtt
    python tools/fetch_idf_submodules.py --idf-path ... --dry-run

Set GITHUB_TOKEN to raise the API rate limit (60/hour unauthenticated).
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.error
import urllib.request
from pathlib import Path

USER_AGENT = "IO_Agent-submodule-fetcher/1.0"
CODELOAD = "https://codeload.github.com/{owner}/{repo}/tar.gz/{sha}"
API_COMMIT = "https://api.github.com/repos/{owner}/{repo}/git/commits/{sha}"
API_TREE = "https://api.github.com/repos/{owner}/{repo}/git/trees/{sha}?recursive=1"

CURL = "curl.exe"


# --------------------------------------------------------------------------- #
# small helpers
# --------------------------------------------------------------------------- #
def log(msg: str) -> None:
    print(msg, flush=True)


def run_git(args: list[str], cwd: Path) -> str:
    proc = subprocess.run(
        ["git", *args], cwd=str(cwd), capture_output=True, text=True, errors="replace"
    )
    if proc.returncode != 0:
        raise RuntimeError(
            "git {} failed in {}:\n{}".format(" ".join(args), cwd, proc.stderr.strip())
        )
    return proc.stdout


def git_blob_sha1(data: bytes) -> str:
    """Git blob object id = sha1("blob <len>\\0" + content)."""
    h = hashlib.sha1()
    h.update(b"blob %d\x00" % len(data))
    h.update(data)
    return h.hexdigest()


def owner_repo_from_url(url: str) -> tuple[str, str] | None:
    """Map a submodule URL to (owner, repo) on github.com."""
    url = url.strip()
    m = re.match(r"^(?:https?://)?(?:[^@/]+@)?github\.com[:/]([^/]+)/(.+?)(?:\.git)?/?$", url)
    if not m:
        return None
    return m.group(1), m.group(2)


def resolve_relative_url(parent_url: str, url: str) -> str:
    """Resolve a relative submodule URL against the parent repository URL.

    ESP-IDF's .gitmodules uses paths like ``../../espressif/esp-mqtt.git``,
    which git resolves relative to the parent repository's own remote URL
    (``https://github.com/espressif/esp-idf.git`` -> ``https://github.com/espressif/esp-mqtt.git``).
    """
    if re.match(r"^(?:https?://|git@|ssh://|git://|file://)", url):
        return url
    m = re.match(r"^(https?://[^/]+)(/.*)?$", parent_url)
    if not m:
        return url
    scheme_host, parent_path = m.group(1), m.group(2) or "/"

    parts = [p for p in url.split("/") if p]
    ups = 0
    while parts and parts[0] == "..":
        ups += 1
        parts.pop(0)

    segs = [s for s in parent_path.rstrip("/").split("/") if s]
    segs = segs[:-1]  # drop the parent repository name itself
    for _ in range(ups):
        if segs:
            segs.pop()
    return scheme_host + "/" + "/".join(segs + parts)


# --------------------------------------------------------------------------- #
# enumeration
# --------------------------------------------------------------------------- #
def list_gitlinks(idf: Path) -> list[tuple[str, str]]:
    """Return [(submodule_path, pinned_sha)] for every gitlink in the IDF tree."""
    out = run_git(["ls-tree", "-r", "HEAD"], idf)
    links: list[tuple[str, str]] = []
    for line in out.splitlines():
        if not line.startswith("160000 "):
            continue
        meta, path = line.split("\t", 1)
        parts = meta.split()
        links.append((path.strip(), parts[2]))
    return links


def list_submodule_urls(idf: Path) -> dict[str, str]:
    """Return {submodule_path: url} parsed from .gitmodules."""
    gm = idf / ".gitmodules"
    urls: dict[str, str] = {}
    if not gm.is_file():
        return urls
    section = None
    for raw in gm.read_text(encoding="utf-8", errors="replace").splitlines():
        line = raw.strip()
        if not line or line.startswith(("#", ";")):
            continue
        if line.startswith("[") and line.endswith("]"):
            section = line[1:-1].strip()
            continue
        if "=" not in line or section is None:
            continue
        key, value = line.split("=", 1)
        key = key.strip().lower()
        value = value.strip()
        if key == "path":
            urls.setdefault("_pending_path_", value)
        elif key == "url":
            pending = urls.pop("_pending_path_", None)
            if pending:
                urls[pending] = value
    return urls


# --------------------------------------------------------------------------- #
# download / extract
# --------------------------------------------------------------------------- #
def download_with_resume(url: str, dest: Path, attempts: int = 8) -> int:
    """Download `url` to `dest`, resuming a partial file. Returns byte count."""
    dest.parent.mkdir(parents=True, exist_ok=True)
    last_size = -1
    for attempt in range(1, attempts + 1):
        size_before = dest.stat().st_size if dest.is_file() else 0
        # A previous attempt may have left a truncated file behind. Resuming
        # such a leftover is a trap: the server then answers 200 instead of
        # 206 and curl refuses the file with "HTTP server does not seem to
        # support byte ranges", so every later retry fails the same way. If the
        # last attempt made no progress, drop the leftover and start over.
        if size_before > 0 and size_before == last_size:
            dest.unlink(missing_ok=True)
            size_before = 0
        cmd = [
            CURL, "-L", "--fail", "--silent", "--show-error",
            "--retry", "5", "--retry-delay", "2", "--retry-all-errors",
            "-C", "-",                       # resume from existing partial file
            "-o", str(dest), url,
        ]
        proc = subprocess.run(cmd, capture_output=True, text=True, errors="replace")
        if proc.returncode == 0 and dest.is_file() and dest.stat().st_size > 0:
            return dest.stat().st_size
        err = (proc.stderr or proc.stdout).strip()[:160]
        log("    ! attempt {}/{} failed: {}".format(attempt, attempts, err))
        if proc.returncode == 33:
            # 33 = server does not support byte ranges; the partial file cannot
            # be resumed, so remove it before the next attempt.
            dest.unlink(missing_ok=True)
        last_size = dest.stat().st_size if dest.is_file() else 0
        if attempt < attempts:
            import time
            time.sleep(2)
    raise RuntimeError("download failed after {} attempts: {}".format(attempts, url))


def extract_tarball(tarball: Path, dest: Path) -> None:
    """Extract a codeload tarball into `dest`, stripping the top-level dir."""
    dest.mkdir(parents=True, exist_ok=True)
    with tarfile.open(tarball, "r:gz") as tf:
        members = tf.getmembers()
        for member in members:
            parts = Path(member.name).parts
            if len(parts) < 2:
                continue  # the single top-level directory entry
            member.name = str(Path(*parts[1:]))
            if not member.name or member.name == ".":
                continue
            try:
                # filter="tar" keeps permission bits and symlinks (needed to
                # reproduce the upstream git tree); "data" would strip them.
                try:
                    tf.extract(member, dest, set_attrs=True, filter="tar")
                except TypeError:  # Python < 3.11.4 has no extraction filters
                    tf.extract(member, dest, set_attrs=True)
            except (PermissionError, OSError) as exc:
                raise RuntimeError(
                    "failed to extract {!r}: {}".format(member.name, exc)
                ) from exc


# --------------------------------------------------------------------------- #
# verification
# --------------------------------------------------------------------------- #
def api_get_json(url: str, token: str | None, attempts: int = 6) -> dict:
    """GET a GitHub API JSON document, retrying through proxy hiccups."""
    import time

    last: Exception | None = None
    for attempt in range(1, attempts + 1):
        req = urllib.request.Request(
            url, headers={"User-Agent": USER_AGENT, "Accept": "application/vnd.github+json"}
        )
        if token:
            req.add_header("Authorization", "Bearer " + token)
        try:
            with urllib.request.urlopen(req, timeout=60) as resp:
                return json.loads(resp.read().decode("utf-8"))
        except Exception as exc:  # noqa: BLE001 - proxy can drop the connection
            last = exc
            if attempt < attempts:
                time.sleep(1.5 * attempt)
    raise RuntimeError("GitHub API {} failed after {} attempts: {}".format(
        url, attempts, last))


def upstream_tree(owner: str, repo: str, sha: str, token: str | None) -> tuple[dict[str, str], bool]:
    """Return ({path: (mode, sha)}, truncated) for the upstream commit tree."""
    commit = api_get_json(API_COMMIT.format(owner=owner, repo=repo, sha=sha), token)
    tree_sha = commit["tree"]["sha"]
    tree = api_get_json(API_TREE.format(owner=owner, repo=repo, sha=tree_sha), token)
    entries: dict[str, tuple[str, str]] = {}
    for item in tree.get("tree", []):
        # Only blobs have content on disk. Directories (type "tree") and
        # nested gitlinks (type "commit") are handled separately.
        if item.get("type") != "blob":
            continue
        entries[item["path"]] = (item["mode"], item["sha"])
    return entries, bool(tree.get("truncated"))


def verify_tree(dest: Path, upstream: dict[str, tuple[str, str]]) -> tuple[int, int, list[str]]:
    """Compare the extracted tree against the upstream tree listing.

    Returns (checked, mismatched, problems). Symlinks are compared by their
    target text, which is exactly what git stores as the blob content.
    """
    checked = 0
    problems: list[str] = []
    for rel, (mode, want_sha) in upstream.items():
        if mode == "160000":
            continue  # nested gitlink: codeload tarball carries no content
        local = dest / rel
        if not local.is_file() and not local.is_symlink():
            problems.append("missing: " + rel)
            continue
        if mode == "120000":
            data = os.readlink(local).encode("utf-8")
        else:
            data = local.read_bytes()
        got = git_blob_sha1(data)
        checked += 1
        if got != want_sha:
            problems.append("sha mismatch: {} (want {}, got {})".format(rel, want_sha, got))
    return checked, len(problems), problems


def local_file_set(dest: Path) -> set[str]:
    out: set[str] = set()
    for root, dirs, files in os.walk(dest):
        dirs[:] = [d for d in dirs if d != ".git"]
        for name in files:
            p = Path(root) / name
            out.add(str(p.relative_to(dest)).replace("\\", "/"))
    return out


# --------------------------------------------------------------------------- #
# main
# --------------------------------------------------------------------------- #
def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--idf-path", required=True, type=Path)
    ap.add_argument("--only", action="append", default=[],
                    help="submodule path to fetch (repeatable); default = all")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--no-verify", action="store_true", help="skip GitHub tree verification")
    ap.add_argument("--double-check", action="store_true",
                    help="verify by downloading the tarball twice and comparing sha256 "
                         "(independent of the GitHub API rate limit)")
    ap.add_argument("--verify-existing", action="store_true",
                    help="only verify trees already present on disk; download nothing")
    ap.add_argument("--keep-tarballs", action="store_true")
    args = ap.parse_args()

    idf = args.idf_path.resolve()
    if not (idf / ".git").exists():
        log("ERROR: {} is not a git repository".format(idf))
        return 2

    token = os.environ.get("GITHUB_TOKEN") or None
    links = list_gitlinks(idf)
    urls = list_submodule_urls(idf)
    try:
        parent_url = run_git(["remote", "get-url", "origin"], idf).strip()
    except Exception:  # noqa: BLE001
        parent_url = ""
    if parent_url:
        urls = {p: resolve_relative_url(parent_url, u) for p, u in urls.items()}

    wanted = set(args.only) if args.only else None
    if wanted:
        links = [(p, s) for p, s in links if p in wanted]
    if not links:
        log("nothing to do")
        return 0

    tmp_root = Path(tempfile.mkdtemp(prefix="idf_subs_"))
    failures: list[str] = []
    results: list[tuple[str, str, int, int, bool]] = []

    try:
        for path, sha in links:
            url = urls.get(path)
            if not url:
                log("SKIP {}: no URL in .gitmodules".format(path))
                continue
            parsed = owner_repo_from_url(url)
            if not parsed:
                log("SKIP {}: unsupported URL {}".format(path, url))
                continue
            owner, repo = parsed

            if args.dry_run:
                log("PLAN {} <- {}/{} @ {}".format(path, owner, repo, sha[:12]))
                continue

            log("FETCH {} <- {}/{} @ {}".format(path, owner, repo, sha[:12]))
            dest = idf / path

            if args.double_check:
                # Independent of the GitHub API: fetch the same commit twice
                # through two separate HTTP transfers and require the bytes to
                # be identical. The proxy damages transfers at random, so two
                # agreeing transfers are strong evidence of an intact payload.
                digests = []
                tarball = None
                try:
                    for attempt in range(1, 5):
                        cand = tmp_root / ("dc{}_{}.tar.gz".format(attempt, repo))
                        download_with_resume(
                            CODELOAD.format(owner=owner, repo=repo, sha=sha), cand)
                        digests.append(hashlib.sha256(cand.read_bytes()).hexdigest())
                        if attempt == 1:
                            tarball = cand
                        else:
                            cand.unlink(missing_ok=True)
                        if len(set(digests)) == 1:
                            break
                    if len(set(digests)) != 1:
                        raise RuntimeError("two downloads disagree: " + ", ".join(digests))
                except Exception as exc:  # noqa: BLE001
                    log("  FAIL double-check: {}".format(exc))
                    failures.append(path)
                    continue

                staging = tmp_root / ("stage_" + repo)
                if staging.exists():
                    shutil.rmtree(staging)
                try:
                    extract_tarball(tarball, staging)
                except Exception as exc:  # noqa: BLE001
                    log("  FAIL extract: {}".format(exc))
                    failures.append(path)
                    continue
                if dest.exists():
                    shutil.rmtree(dest)
                dest.parent.mkdir(parents=True, exist_ok=True)
                shutil.move(str(staging), str(dest))
                log("  OK {} (two independent downloads agree, sha256={})".format(
                    path, digests[0][:16]))
                results.append((path, sha, tarball.stat().st_size, 0, False))
                continue

            if args.verify_existing:
                if not dest.is_dir():
                    log("  FAIL not present on disk")
                    failures.append(path)
                    continue
                try:
                    upstream, truncated = upstream_tree(owner, repo, sha, token)
                    checked, bad, problems = verify_tree(dest, upstream)
                    extra = local_file_set(dest) - set(upstream.keys())
                    for rel in sorted(extra):
                        problems.append("unexpected extra file: " + rel)
                        bad += 1
                except Exception as exc:  # noqa: BLE001
                    log("  FAIL verify: {}".format(exc))
                    failures.append(path)
                    continue
                if bad:
                    log("  FAIL verify: {} problem(s)".format(bad))
                    for p in problems[:8]:
                        log("      - " + p)
                    failures.append(path)
                    continue
                log("  OK {} (verified {}/{} files{})".format(
                    path, checked, checked + bad, ", partial" if truncated else ""))
                results.append((path, sha, 0, checked, truncated))
                continue

            tarball = tmp_root / (repo + "-" + sha[:12] + ".tar.gz")
            try:
                size = download_with_resume(CODELOAD.format(owner=owner, repo=repo, sha=sha), tarball)
            except Exception as exc:  # noqa: BLE001
                log("  FAIL download: {}".format(exc))
                failures.append(path)
                continue

            dest = idf / path
            staging = tmp_root / ("stage_" + repo)
            if staging.exists():
                shutil.rmtree(staging)
            try:
                extract_tarball(tarball, staging)
            except Exception as exc:  # noqa: BLE001
                log("  FAIL extract: {}".format(exc))
                failures.append(path)
                continue

            checked, bad, problems = 0, 0, []
            truncated = False
            if not args.no_verify:
                try:
                    upstream, truncated = upstream_tree(owner, repo, sha, token)
                    checked, bad, problems = verify_tree(staging, upstream)
                    extra = local_file_set(staging) - set(upstream.keys())
                    for rel in sorted(extra):
                        problems.append("unexpected extra file: " + rel)
                        bad += 1
                except Exception as exc:  # noqa: BLE001
                    log("  WARN verification unavailable: {}".format(exc))
                    problems = []
            else:
                log("  (verification skipped)")

            if bad:
                log("  FAIL verify: {} problem(s)".format(bad))
                for p in problems[:8]:
                    log("      - " + p)
                failures.append(path)
                continue

            # Replace the target directory with the verified content.
            if dest.exists():
                shutil.rmtree(dest)
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.move(str(staging), str(dest))

            note = "verified {}/{} files".format(checked, checked + bad) if checked else "unverified"
            if truncated:
                note += " (upstream tree listing truncated)"
            log("  OK {} ({}, {} bytes)".format(path, note, size))
            results.append((path, sha, size, checked, truncated))

            if not args.keep_tarballs:
                tarball.unlink(missing_ok=True)
    finally:
        if not args.keep_tarballs:
            shutil.rmtree(tmp_root, ignore_errors=True)

    log("")
    log("summary: {} fetched, {} failed".format(len(results), len(failures)))
    for path, sha, size, checked, truncated in results:
        log("  + {} @ {} ({} bytes, {} files verified{})".format(
            path, sha[:12], size, checked, ", partial" if truncated else ""))
    for path in failures:
        log("  - {} FAILED".format(path))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
