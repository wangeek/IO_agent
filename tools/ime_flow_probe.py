#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""ime_flow_probe.py -- 验收"探针式 IME 处理"（2026-10-23）。

新流程（设备端**零自动重试**，重试由 agent 决定）：
    act macro=selftest        -> INJ.TXT 出现 = 当时 IME 在英文态
    （没出现）act macro=ime_toggle   -> 单按一次 Shift 切输入法态
    act macro=selftest        -> 再验一次
    act macro=launch_watcher  -> 设备内部先探针再打启动命令 -> CAP.TXT

用法:
    python tools/ime_flow_probe.py 172.17.21.225
"""

import json
import os
import sys
import time
import urllib.request

TIMEOUT = 30
VOL = r"E:\INJ.TXT"
CAP = r"E:\CAP.TXT"


def opener():
    return urllib.request.build_opener(urllib.request.ProxyHandler({}))


def rpc(url, method, params=None, rpc_id=1):
    body = {"jsonrpc": "2.0", "id": rpc_id, "method": method}
    if params is not None:
        body["params"] = params
    data = json.dumps(body).encode("utf-8")
    req = urllib.request.Request(url, data=data, method="POST",
                                 headers={"Content-Type": "application/json"})
    with opener().open(req, timeout=TIMEOUT) as r:
        return json.loads(r.read().decode("utf-8", "replace"))


def call(url, name, args=None):
    obj = rpc(url, "tools/call", {"name": name, "arguments": args or {}})
    result = obj.get("result", {})
    if "content" in result and result["content"]:
        text = result["content"][0].get("text", "")
        try:
            return json.loads(text)
        except Exception:
            return {"raw": text}
    return obj


def wait_idle(url, timeout=60):
    t0 = time.time()
    while time.time() - t0 < timeout:
        info = call(url, "device/info")
        if not info.get("busy"):
            return time.time() - t0
        time.sleep(0.3)
    return -1.0


def mtime(path):
    try:
        return os.path.getmtime(path)
    except OSError:
        return 0.0


def act_macro(url, macro, **extra):
    args = {"action": "macro", "macro": macro}
    args.update(extra)
    t0 = time.time()
    r = call(url, "act", args)
    return (time.time() - t0) * 1000.0, r


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    host = sys.argv[1]
    os.environ["no_proxy"] = host + "," + os.environ.get("no_proxy", "")
    os.environ["NO_PROXY"] = os.environ["no_proxy"]
    url = "http://%s:8080/mcp" % host

    print("=== 1) probe the IME state (act macro=selftest, no Shift) ===")
    english = False
    before = mtime(VOL)
    ms, r = act_macro(url, "selftest")
    print("  accepted in %.0f ms -> %s" % (ms, r.get("accepted")))
    wait_idle(url)
    if mtime(VOL) > before:
        print("  -> INJ.TXT written: the IME is in ENGLISH mode")
        english = True
    else:
        print("  -> no INJ.TXT: the IME is in CHINESE mode (the command was mangled)")

    # 新开的运行框**继承**当前态,所以 launch 的档位 = 探针结论的反面:
    #   探针(不按)过了  -> 当前英文 -> launch 也不按
    #   探针(不按)没过  -> 当前中文 -> launch 按一次 Shift(新框继承中文 -> 切到英文)
    print("=== 2) launch_watcher (ime=%s) ===" % ("none" if english else "toggle"))
    before = mtime(CAP)
    extra = {} if english else {"ime": "toggle"}
    ms, r = act_macro(url, "launch_watcher", **extra)
    print("  accepted in %.0f ms -> %s" % (ms, r.get("accepted")))
    dt = wait_idle(url)
    print("  slot free after %.1f s" % dt if dt >= 0 else "  TIMEOUT")
    after = mtime(CAP)
    print("  CAP.TXT %s" % ("appeared -> the watcher is up" if after > before else "MISSING"))

    print("=== 3) read tools still served ===")
    ms, ws = call(url, "watcher/status"), None
    print("  watcher/status: %s" % (ms[0] if isinstance(ms, tuple) else ms))
    print("PASS" if after > before else "FAIL")
    return 0 if after > before else 1


if __name__ == "__main__":
    sys.exit(main())
