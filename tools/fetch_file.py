#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""fetch_file.py -- 从 Sophon 设备拉一个文件到本地(原始字节)。

设备现在按**文件名**给原始字节:GET /files/<name>。
以前这条路叫 /blob/b0..b3 —— 那是把"内部缓存槽位"暴露成了对外契约,
现在文件名就是地址,不用先 observe 再取。

用法:
    python tools/fetch_file.py 172.17.21.225 SNAP_P.JPG out.jpg
    python tools/fetch_file.py 172.17.21.225 /files/SNAP_P.JPG out.jpg
    python tools/fetch_file.py 172.17.21.225 http://172.17.21.225:8080/files/SNAP_P.JPG out.jpg
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
        if not url.startswith("/"):
            url = "/files/%s" % url
        url = "http://%s:8080%s" % (host, url)

    # Make sure the proxy never sees the device.
    os.environ["no_proxy"] = host + "," + os.environ.get("no_proxy", "")
    os.environ["NO_PROXY"] = os.environ["no_proxy"]
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))

    req = urllib.request.Request(url, headers={"User-Agent": "sophon-fetch/1"})
    try:
        with opener.open(req, timeout=TIMEOUT) as r:
            data = r.read()
            ctype = r.headers.get("Content-Type", "?")
    except urllib.error.HTTPError as e:
        print("HTTP %d: %s" % (e.code, e.reason))
        return 1
    with open(out, "wb") as f:
        f.write(data)
    print("saved %d bytes (%s) -> %s" % (len(data), ctype, out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
