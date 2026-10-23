#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Fetch a blob (or any URL) from the device and save it to a local file.

The device returns blob URLs like http://<ip>:8080/blob/b0 from the observe
tool. This helper pulls the bytes down so they can be inspected locally.

Proxy note: this host has http_proxy set (Clash Verge). urllib honours it,
so the device address must be in no_proxy or the fetch goes to the proxy
and fails. The caller is expected to set no_proxy, but we also strip the
proxy explicitly for the device host to be safe.

Usage:
    python tools/fetch_blob.py 172.17.21.225 http://172.17.21.225:8080/blob/b0 out.jpg
    python tools/fetch_blob.py 172.17.21.225 /blob/b0 out.jpg
"""

import os
import sys
import urllib.request

TIMEOUT = 20


def main():
    if len(sys.argv) != 4:
        print(__doc__)
        return 2
    host, url, out = sys.argv[1], sys.argv[2], sys.argv[3]
    if not url.startswith("http"):
        url = "http://%s:8080%s" % (host, url)

    # Make sure the proxy never sees the device.
    os.environ["no_proxy"] = host + "," + os.environ.get("no_proxy", "")
    os.environ["NO_PROXY"] = os.environ["no_proxy"]
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))

    req = urllib.request.Request(url, headers={"User-Agent": "ioagent-fetch/1"})
    with opener.open(req, timeout=TIMEOUT) as r:
        data = r.read()
        ctype = r.headers.get("Content-Type", "?")
    with open(out, "wb") as f:
        f.write(data)
    print("saved %d bytes (%s) -> %s" % (len(data), ctype, out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
