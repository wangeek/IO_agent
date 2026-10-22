"""Check whether the local HTTP proxy serves wheel bytes that match PyPI's official digests.

Usage:
    python tools/check_proxy_integrity.py [package ...]

With no arguments it checks `pip` itself. For each package it:
  1. queries the PyPI JSON API for the official sha256 of the newest wheel,
  2. downloads that wheel twice -- once through the proxy env vars, once with
     the proxy disabled,
  3. compares both local digests against the official digest.

Exit code 0 means every download matched, 1 means at least one mismatch.
"""

import hashlib
import http.client
import json
import os
import sys
import urllib.request

CHUNK = 1 << 16


def pypi_digest_and_url(package):
    url = f"https://pypi.org/pypi/{package}/json"
    data = json.loads(fetch(url, True))
    best = None
    for rel in data["releases"].values():
        for f in rel:
            if f["packagetype"] != "bdist_wheel":
                continue
            if best is None or f["upload_time_iso_8601"] > best["upload_time_iso_8601"]:
                best = f
    if best is None:
        raise RuntimeError(f"no wheel found for {package}")
    return best["digests"]["sha256"], best["url"], best["filename"]


def _opener(use_proxy):
    # ProxyHandler({}) strips every proxy setting for this opener only.
    # An empty ProxyHandler() falls back to the environment variables.
    if use_proxy:
        return urllib.request.build_opener(urllib.request.ProxyHandler())
    return urllib.request.build_opener(urllib.request.ProxyHandler({}))


def fetch(url, use_proxy, attempts=6):
    """Fetch url, resuming with Range when the peer truncates the response."""
    opener = _opener(use_proxy)
    buf = bytearray()
    for _ in range(attempts):
        req = urllib.request.Request(url)
        if buf:
            req.add_header("Range", f"bytes={len(buf)}-")
        try:
            with opener.open(req, timeout=120) as resp:
                if buf and resp.status != 206:
                    # Server ignored Range: restart cleanly.
                    buf = bytearray()
                while True:
                    chunk = resp.read(CHUNK)
                    if not chunk:
                        break
                    buf.extend(chunk)
        except http.client.IncompleteRead as exc:
            buf.extend(exc.partial)
            if len(buf) >= expected_size(url, use_proxy):
                break
            continue
        except Exception:
            if buf:
                continue
            raise
        break
    return bytes(buf)


def expected_size(url, use_proxy):
    opener = _opener(use_proxy)
    req = urllib.request.Request(url, method="HEAD")
    with opener.open(req, timeout=60) as resp:
        return int(resp.headers.get("Content-Length") or 0)


def download(url, use_proxy):
    return hashlib.sha256(fetch(url, use_proxy)).hexdigest()


def main(argv):
    packages = argv[1:] or ["pip"]
    proxy = os.environ.get("HTTPS_PROXY") or os.environ.get("https_proxy") or "(none)"
    print(f"proxy env: {proxy}")
    failures = 0
    for pkg in packages:
        try:
            official, url, filename = pypi_digest_and_url(pkg)
        except Exception as exc:  # noqa: BLE001 - report and continue
            print(f"{pkg}: could not query PyPI: {exc}")
            failures += 1
            continue
        print(f"\n{pkg} -> {filename}")
        print(f"  official sha256 : {official}")
        for label, use_proxy in (("via-proxy", True), ("direct", False)):
            try:
                got = download(url, use_proxy)
            except Exception as exc:  # noqa: BLE001 - report and continue
                print(f"  {label:10s}: download failed: {exc}")
                failures += 1
                continue
            mark = "OK " if got == official else "BAD"
            print(f"  {label:10s}: {mark} {got}")
            if got != official:
                failures += 1
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
