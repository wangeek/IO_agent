#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""mtime_probe.py -- 回归"设备读到的最后写入时间是不是真的"。

背景(2026-10-24):`io_disk.c` 把 FAT16 目录项里的"最后写入时间/日期"
读成了 0x24/0x26 —— 那已经越过本条目 32 字节,读到的是**下一个条目**
的字节。症状很隐蔽:读出来的值是**合法的日期**(1995 年),看着挺合理,
于是 `ls` 的 mtime 全错,而"挑最新一帧"是按 mtime 排的 ⇒ 挑帧逻辑跟着错。

正确偏移(FAT16 目录项 32 字节):
    0x16-0x17  最后写入时间
    0x18-0x19  最后写入日期

本探针验三件事:
  M1 设备报的 mtime 与宿主自己看到的同一时刻**一致**(差一个固定时区偏移 +
     FAT 的 2 秒粒度)
  M2 **变更能被看见**:宿主重写同一个文件后,设备看到的 mtime 必须变
     —— 这正是注入自检"这次写的"判据所依赖的能力
  M3 **先后能排对**:宿主先写 A、再写 B(间隔 >2 秒),设备必须认为 B 更新

用法:
    python tools/mtime_probe.py 172.17.21.225
    python tools/mtime_probe.py 172.17.21.225 --volume E:
"""

import argparse
import json
import os
import sys
import time
import urllib.request

TIMEOUT = 10
RESULTS = []

# Minimal JPEG payloads: the device decides "is this an image?" by the magic
# bytes, so a header plus filler is enough for the selection test.
JPEG_OLD = b"\xff\xd8\xff\xe0" + b"OLD-FRAME" * 16 + b"\xff\xd9"
JPEG_NEW = b"\xff\xd8\xff\xe0" + b"NEW-FRAME" * 16 + b"\xff\xd9"


def opener():
    for var in ("http_proxy", "https_proxy", "HTTP_PROXY", "HTTPS_PROXY",
                "ALL_PROXY", "all_proxy"):
        os.environ.pop(var, None)
    return urllib.request.build_opener(urllib.request.ProxyHandler({}))


def call(ip, tool, args=None):
    body = {"jsonrpc": "2.0", "id": 1, "method": "tools/call",
            "params": {"name": tool, "arguments": args or {}}}
    req = urllib.request.Request("http://%s:8080/mcp" % ip,
                                 data=json.dumps(body).encode("utf-8"),
                                 method="POST")
    req.add_header("Content-Type", "application/json")
    with opener().open(req, timeout=TIMEOUT) as r:
        env = json.loads(r.read().decode("utf-8", "replace"))
    if "error" in env:
        raise RuntimeError(env["error"].get("message", ""))
    return env["result"]["structuredContent"]


def check(rule, desc, got, want):
    ok = (got == want)
    RESULTS.append((rule, desc, got, want, ok))
    print("  [%s] %-4s %-50s got=%-22s want=%s" % (
        "PASS" if ok else "FAIL", rule, desc, str(got)[:22], want))
    return ok


def check_true(rule, desc, got, want_bool):
    ok = (bool(got) == want_bool)
    RESULTS.append((rule, desc, got, want_bool, ok))
    print("  [%s] %-4s %-50s got=%-22s want=%s" % (
        "PASS" if ok else "FAIL", rule, desc, str(got)[:22], want_bool))
    return ok


def device_files(ip):
    """name -> mtime, as the device reports it."""
    return {e["name"]: e["mtime"] for e in call(ip, "ls")["files"]}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ip")
    ap.add_argument("--volume", default="E:", help="the dongle's drive letter")
    ap.add_argument("--prefix", default="MTIME.TXT", help="probe file name (8.3)")
    args = ap.parse_args()

    ip = args.ip
    vol = args.volume
    print("=== mtime probe: %s (volume %s) ===" % (ip, vol))

    # ---- M1: device mtime vs host mtime ----
    # FAT stores *local* wall-clock time and the firmware interprets it as UTC,
    # so the two differ by a constant offset (the host's UTC offset). We do not
    # care about the offset itself -- what matters is that it is CONSTANT and
    # that the device is not reading the wrong bytes entirely.
    host = {}
    for name in os.listdir(vol):
        p = os.path.join(vol, name)
        try:
            host[name.upper()] = int(os.path.getmtime(p))
        except OSError:
            pass
    dev = device_files(ip)
    common = [n for n in host if n in dev and host[n] and dev[n]]
    if not common:
        print("  (no comparable file)")
    else:
        offsets = [dev[n] - host[n] for n in common]
        spread = max(offsets) - min(offsets)
        check_true("M1", "device/host mtime offset is constant across files",
                   spread <= 2, True)
        off = offsets[0]
        print("       (host->device offset = %+d s = %+d h; FAT local time read as UTC)"
              % (off, off // 3600))

    # ---- M2: a rewrite must be visible ----
    p = os.path.join(vol, args.prefix)
    try:
        with open(p, "wb") as f:
            f.write(b"PROBE_V1")
        time.sleep(3.0)          # > FAT's 2-second granularity
        m1 = device_files(ip).get(args.prefix.upper())
        with open(p, "wb") as f:
            f.write(b"PROBE_V2_DIFFERENT")
        time.sleep(3.0)
        m2 = device_files(ip).get(args.prefix.upper())
        check_true("M2", "a host rewrite changes the device's mtime",
                   (m1 is not None and m2 is not None and m1 != m2), True)
        print("       (mtime %s -> %s)" % (m1, m2))
    except OSError as exc:
        print("  [FAIL] M2  cannot write to %s: %s" % (p, exc))
        RESULTS.append(("M2", "rewrite", str(exc), "changed", False))

    # ---- M3: ordering must be right (this is what "newest frame" relies on) ----
    a = os.path.join(vol, "MTIMEA.TXT")
    b = os.path.join(vol, "MTIMEB.TXT")
    try:
        with open(a, "wb") as f:
            f.write(b"A")
        time.sleep(3.0)
        with open(b, "wb") as f:
            f.write(b"B")
        time.sleep(2.0)
        dev = device_files(ip)
        ma = dev.get("MTIMEA.TXT")
        mb = dev.get("MTIMEB.TXT")
        check_true("M3", "the file written later has the larger mtime",
                   (ma is not None and mb is not None and mb > ma), True)
        print("       (A=%s  B=%s)" % (ma, mb))
    except OSError as exc:
        print("  [FAIL] M3  %s" % exc)
        RESULTS.append(("M3", "ordering", str(exc), "B>A", False))

    # ---- M4: capture must pick the NEWEST frame (the thing mtime is for) ----
    # The frames are written by the host here (fake JPEGs), so this needs no
    # injection and no watcher -- it exercises exactly the selection logic.
    older = os.path.join(vol, "SNAP_P.JPG")
    newer = os.path.join(vol, "SNAP_A.JPG")
    try:
        with open(older, "wb") as f:
            f.write(JPEG_OLD)
        time.sleep(3.0)
        with open(newer, "wb") as f:
            f.write(JPEG_NEW)
        time.sleep(2.0)
        res = call(ip, "capture")
        picked = res.get("file")
        check("M4", "capture picks the frame written last", picked, "SNAP_A.JPG")
    except OSError as exc:
        print("  [FAIL] M4  %s" % exc)
        RESULTS.append(("M4", "capture picks the newest", str(exc), "SNAP_A.JPG", False))

    # ---- leave the volume as we found it ----
    for name in (args.prefix, "MTIMEA.TXT", "MTIMEB.TXT",
                 "SNAP_P.JPG", "SNAP_A.JPG"):
        try:
            os.remove(os.path.join(vol, name))
        except OSError:
            pass

    failed = [r for r in RESULTS if not r[4]]
    print("=== summary ===")
    print("  %d checks, %d passed, %d failed" % (
        len(RESULTS), len(RESULTS) - len(failed), len(failed)))
    for r in failed:
        print("  FAIL %s: %s (got %s want %s)" % (r[0], r[1], r[2], r[3]))
    return 0 if not failed else 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as exc:  # noqa: BLE001
        print("ERROR: %s" % exc)
        sys.exit(2)
