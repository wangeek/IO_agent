#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""observe -> download -> local file, in one shot.

Calls the device's observe tool (which copies a file off the target volume
into a blob slot) and then pulls the blob URL down to a local file so the
frame can be inspected.

Proxy note: this host has http_proxy set (Clash Verge). Both the JSON-RPC
call and the blob download must bypass it, otherwise the device request is
sent to the proxy and fails.

Usage:
    python tools/observe_fetch.py 172.17.21.225 out.jpg [preview|full|roi|diff|<8.3 name>]
"""

import json
import os
import sys
import urllib.request

TIMEOUT = 20


def opener():
    # Explicitly empty proxy handler: never route device traffic to a proxy.
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


def call_tool(url, name, args=None):
    obj = rpc(url, "tools/call", {"name": name, "arguments": args or {}})
    result = obj.get("result", {})
    if "content" in result and result["content"]:
        text = result["content"][0].get("text", "")
        try:
            return json.loads(text)
        except Exception:
            return text
    return obj


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    host, out = sys.argv[1], sys.argv[2]
    target = sys.argv[3] if len(sys.argv) > 3 else "preview"
    os.environ["no_proxy"] = host + "," + os.environ.get("no_proxy", "")
    os.environ["NO_PROXY"] = os.environ["no_proxy"]

    url = "http://%s:8080/mcp" % host
    res = call_tool(url, "observe", {"target": target})
    print(json.dumps(res, ensure_ascii=False))
    if not isinstance(res, dict) or not res.get("ok") or not res.get("url"):
        return 1

    with opener().open(urllib.request.Request(res["url"]), timeout=TIMEOUT) as r:
        data = r.read()
    with open(out, "wb") as f:
        f.write(data)
    print("saved %d bytes -> %s" % (len(data), out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
