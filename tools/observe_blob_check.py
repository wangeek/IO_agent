#!/usr/bin/env python3
"""Is the observe handler hanging, or is only the response lost?

If the blob slot ends up populated after the observe call times out, the
handler DID finish and the problem is in the response path. If the slot is
empty, the handler itself is stuck.
"""
import sys
import time
import urllib.request

HOST = sys.argv[1] if len(sys.argv) > 1 else "172.17.21.225"
TARGET = sys.argv[2] if len(sys.argv) > 2 else "TEST.TXT"
BASE = "http://%s:8080" % HOST
opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))

body = ('{"jsonrpc":"2.0","id":1,"method":"tools/call","params":'
        '{"name":"observe","arguments":{"target":"%s"}}}' % TARGET).encode()
req = urllib.request.Request(BASE + "/mcp", data=body, method="POST",
                             headers={"Content-Type": "application/json"})

t0 = time.time()
try:
    r = opener.open(req, timeout=20)
    print("observe  HTTP %d  %.2fs  %s" % (r.status, time.time() - t0, r.read()[:160]))
except Exception as exc:  # noqa: BLE001
    print("observe  FAILED after %.2fs: %s %s" % (time.time() - t0, type(exc).__name__,
                                                 str(exc)[:70]))

print("--- GET /blob/b0 ---")
t1 = time.time()
try:
    r = opener.open(BASE + "/blob/b0", timeout=10)
    data = r.read()
    print("blob HTTP %d  %.2fs  bytes=%d  %r" % (r.status, time.time() - t1, len(data),
                                                data[:40]))
except Exception as exc:  # noqa: BLE001
    print("blob FAILED after %.2fs: %s %s" % (time.time() - t1, type(exc).__name__,
                                              str(exc)[:70]))
