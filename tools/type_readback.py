#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""type_readback.py -- 往运行框里打一条命令并把**框里的真实文本**读回来。

用来判定"设备打出去的字到底对不对"(IME 吃掉 / 布局错位 / 历史预填没清掉)。

注意:长文本的 type 现在是**异步**的(走 worker task),所以打完必须等
device/info 的 busy=false 才能读,否则读到的是历史预填或半截字。

用法:
    python tools/type_readback.py 172.17.21.225
    python tools/type_readback.py 172.17.21.225 "<要打的文本>"
"""

import json
import os
import sys
import time
import urllib.request

TIMEOUT = 30
DEFAULT_TEXT = ("cmd /c for %d in (I O A G E N T) do @if exist %d:\\WATCH.PS1 "
                "start \"\" powershell -ExecutionPolicy Bypass -WindowStyle Hidden %d:\\WATCH.PS1")


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


def wait_idle(url, timeout=40):
    t0 = time.time()
    while time.time() - t0 < timeout:
        info = call(url, "device/info")
        if not info.get("busy"):
            return time.time() - t0
        time.sleep(0.3)
    return -1.0


def read_runbox_text():
    """读 Windows 运行框(Win11 经典 #32770)里的编辑框文本(跨进程 WM_GETTEXT)。"""
    import ctypes
    from ctypes import wintypes

    user32 = ctypes.windll.user32

    class RECT(ctypes.Structure):
        _fields_ = [("left", ctypes.c_long), ("top", ctypes.c_long),
                    ("right", ctypes.c_long), ("bottom", ctypes.c_long)]

    found = []

    WNDENUMPROC = ctypes.WINFUNCTYPE(ctypes.c_bool, wintypes.HWND, wintypes.LPARAM)

    def class_of(hwnd):
        buf = ctypes.create_unicode_buffer(256)
        user32.GetClassNameW(hwnd, buf, 256)
        return buf.value

    def title_of(hwnd):
        buf = ctypes.create_unicode_buffer(512)
        user32.GetWindowTextW(hwnd, buf, 512)
        return buf.value

    def child_texts(parent):
        out = []

        def cb(hwnd, lparam):
            if class_of(hwnd) == "Edit":
                buf = ctypes.create_unicode_buffer(4096)
                user32.SendMessageW(hwnd, 0x000D, ctypes.c_void_p(4096), buf)
                out.append(buf.value)
            return True

        user32.EnumChildWindows(parent, WNDENUMPROC(cb), 0)
        return out

    def top_cb(hwnd, lparam):
        if user32.IsWindowVisible(hwnd) and class_of(hwnd) == "#32770":
            texts = child_texts(hwnd)
            if texts:
                found.append((title_of(hwnd), texts))
        return True

    user32.EnumWindows(WNDENUMPROC(top_cb), 0)
    return found


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    host = sys.argv[1]
    text = sys.argv[2] if len(sys.argv) > 2 else DEFAULT_TEXT
    os.environ["no_proxy"] = host + "," + os.environ.get("no_proxy", "")
    os.environ["NO_PROXY"] = os.environ["no_proxy"]
    url = "http://%s:8080/mcp" % host

    print("=== 1) focus_reset (Win+M / Win+R / Ctrl+Space / Shift / Ctrl+A) ===")
    r = call(url, "act", {"action": "macro", "macro": "focus_reset"})
    print("   ->", json.dumps(r, ensure_ascii=False)[:160])
    wait_idle(url)

    print("=== 2) type (异步) ===")
    r = call(url, "act", {"action": "type", "text": text})
    print("   ->", json.dumps(r, ensure_ascii=False)[:160])

    print("=== 3) 等动作跑完(busy=false) ===")
    dt = wait_idle(url)
    print("   -> idle after %.1f s" % dt if dt >= 0 else "   -> TIMEOUT")

    print("=== 4) 运行框里的真实文本 ===")
    for title, texts in read_runbox_text():
        print("   dialog title codepoints:", " ".join("U+%04X" % ord(c) for c in title))
        for t in texts:
            if all(0x20 <= ord(c) <= 0x7E for c in t) and t:
                print("   ASCII  :", repr(t))
            else:
                print("   NON-ASCII codepoints:",
                      " ".join("U+%04X" % ord(c) for c in t))
    return 0


if __name__ == "__main__":
    sys.exit(main())
