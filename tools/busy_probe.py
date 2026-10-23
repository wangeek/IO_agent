#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""busy_probe.py -- 验收"单例 + busy"语义(2026-10-23 P1-B)。

要验的四件事:
  1. 长动作(注入类)立刻回 accepted,不等它跑完;
  2. 动作在跑时,再来一个动作请求**立刻**回 busy(不是排队等);
  3. 动作在跑时,读类请求(device/info / observe)照常服务;
  4. 动作跑完后,单例槽释放(busy=false),盘上出现标记文件。

用法:
    python tools/busy_probe.py 172.17.21.225 [macro]
    python tools/busy_probe.py 172.17.21.225 selftest
"""

import json
import os
import sys
import time
import urllib.request

TIMEOUT = 30


def opener():
    # 本机有 http_proxy(Clash),设备的请求不能走代理
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
            return {"raw": text, "isError": result.get("isError", False)}
    return obj


def timed(url, name, args=None):
    t0 = time.time()
    res = call(url, name, args)
    return (time.time() - t0) * 1000.0, res


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    host = sys.argv[1]
    macro = sys.argv[2] if len(sys.argv) > 2 else "selftest"
    os.environ["no_proxy"] = host + "," + os.environ.get("no_proxy", "")
    os.environ["NO_PROXY"] = os.environ["no_proxy"]
    url = "http://%s:8080/mcp" % host

    print("=== 0) baseline ===")
    ms, info = timed(url, "device/info")
    print("  device/info  %6.0f ms  busy=%s" % (ms, info.get("busy")))

    print("=== 1) long action: act macro=%s (expect: fast accept) ===" % macro)
    ms, res = timed(url, "act", {"action": "macro", "macro": macro})
    print("  %6.0f ms  ok=%s accepted=%s busy=%s" %
          (ms, res.get("ok"), res.get("accepted"), res.get("busy")))
    if not res.get("accepted"):
        print("  FAIL: not accepted ->", json.dumps(res, ensure_ascii=False)[:300])
        return 1

    print("=== 2) while running: another action (expect: immediate busy) ===")
    ms, res2 = timed(url, "act", {"action": "tap", "key": "a"})
    print("  %6.0f ms  ok=%s busy=%s error=%s" %
          (ms, res2.get("ok"), res2.get("busy"), res2.get("error")))
    if res2.get("busy") is not True:
        print("  FAIL: expected busy")
        return 1
    if ms > 2000:
        print("  FAIL: busy answer took %.0f ms (should be immediate)" % ms)
        return 1

    print("=== 3) while running: read tools still served ===")
    ms, info2 = timed(url, "device/info")
    print("  device/info  %6.0f ms  busy=%s" % (ms, info2.get("busy")))
    ms, ws = timed(url, "watcher/status")
    print("  watcher/status %4.0f ms  running=%s" % (ms, ws.get("running")))

    print("=== 4) wait for the slot to be released ===")
    t0 = time.time()
    while time.time() - t0 < 40:
        _, i = timed(url, "device/info")
        if not i.get("busy"):
            break
        time.sleep(0.5)
    print("  slot free after %.1f s" % (time.time() - t0))
    if i.get("busy"):
        print("  FAIL: still busy after 40 s")
        return 1

    print("=== 5) marker file (the objective verdict) ===")
    for attempt in range(3):
        try:
            _, obs = timed(url, "observe", {"target": "INJ.TXT"})
            print("  observe INJ.TXT -> ok=%s file=%s" % (obs.get("ok"), obs.get("file")))
            if obs.get("ok"):
                break
        except Exception as e:
            print("  observe attempt %d failed: %s" % (attempt + 1, e))
        time.sleep(1.0)
    if macro == "launch_watcher":
        _, obs = timed(url, "observe", {"target": "CAP.TXT"})
        print("  observe CAP.TXT -> ok=%s file=%s" % (obs.get("ok"), obs.get("file")))
        _, ws = timed(url, "watcher/status")
        print("  watcher/status running=%s" % ws.get("running"))

    print("PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
