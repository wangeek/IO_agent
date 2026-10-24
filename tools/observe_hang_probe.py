#!/usr/bin/env python3
"""Discriminate WHICH observe targets hang the device.

Isolated experiment (device freshly reset, one request at a time):
  1. write a small file from the HOST onto the dongle's virtual disk
  2. observe that host-written file        -> is it "any existing file"?
  3. observe a device slot file            -> is it specific to slots?
  4. observe a missing file                -> control (should be instant)

Usage: python tools/observe_hang_probe.py 172.17.21.225
"""
import os
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request

HOST = sys.argv[1] if len(sys.argv) > 1 else "172.17.21.225"
ENDPOINT = "http://%s:8080/mcp" % HOST
TIMEOUT = 12.0


def find_volume():
    """Find the IOAGENT volume by label (drive letters can change).

    Usage: python tools/observe_hang_probe.py <ip> [drive-letter]
    """
    if len(sys.argv) > 2:
        return sys.argv[2]
    ps = ("Get-Volume | Where-Object { $_.FileSystemLabel -eq 'IOAGENT' } |"
          " Select-Object -First 1 -ExpandProperty DriveLetter")
    try:
        out = subprocess.run(["powershell", "-NoProfile", "-Command", ps],
                             capture_output=True, text=True, timeout=30).stdout
        for tok in out.split():
            if len(tok) == 1 and tok.isalpha():
                return tok + ":"
    except (OSError, subprocess.SubprocessError):
        pass
    return None


def observe(target):
    body = ('{"jsonrpc":"2.0","id":1,"method":"tools/call","params":'
            '{"name":"observe","arguments":{"target":"%s"}}}' % target).encode()
    req = urllib.request.Request(ENDPOINT, data=body, method="POST",
                                 headers={"Content-Type": "application/json"})
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    t0 = time.time()
    try:
        with opener.open(req, timeout=TIMEOUT) as r:
            txt = r.read().decode("utf-8", "replace")
        dt = time.time() - t0
        try:
            inner = txt.split('"text": "', 1)[1].rsplit('"', 1)[0]
            inner = inner.encode().decode("unicode_escape")
        except Exception:
            inner = txt[:120]
        return dt, inner[:150]
    except Exception as exc:  # noqa: BLE001
        return time.time() - t0, "%s: %s" % (type(exc).__name__, str(exc)[:90])


def main():
    vol = find_volume()
    print("volume: %s" % vol)
    if vol:
        path = vol + "\\TEST.TXT"
        try:
            with open(path, "wb") as f:
                f.write(b"HELLO_FROM_HOST_12345")
            print("wrote %s (%d bytes)" % (path, os.path.getsize(path)))
        except OSError as exc:
            print("write failed: %s" % exc)

    for target in ("TEST.TXT", "AGENT.SH", "NOPE.TXT"):
        dt, out = observe(target)
        print("observe %-10s %6.2fs :: %s" % (target, dt, out))
        time.sleep(1.0)


if __name__ == "__main__":
    main()
