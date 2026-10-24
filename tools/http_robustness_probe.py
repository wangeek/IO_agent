#!/usr/bin/env python3
"""Probe the device's HTTP server robustness against misbehaving clients.

Why this exists: the device runs a **single** esp_http_server task with
IDF's default socket pool. An agent (or any client) that opens connections and
does not finish them can take the whole MCP server offline while WiFi/mDNS
keep working, so the device still looks "up" but HTTP is dead.

Phases (each one leaves the device in the state it creates):
  A  hold N idle connections (connect, send nothing), then try a normal call
  B  send a request whose Content-Length promises more than we send
     (partial body), then try a normal call
  C  after releasing everything, try a normal call again (recovery)

Usage:
    python tools/http_robustness_probe.py 172.17.21.225 [--idle 8] [--timeout 20]

Exit codes:
    0  the server stayed (or came back) responsive
    1  the server stopped answering
    2  usage / IO error
"""

import argparse
import json
import socket
import sys
import time
import urllib.error
import urllib.request

PING = b'{"jsonrpc":"2.0","id":1,"method":"ping"}'
HDRS = b"Content-Type: application/json\r\nMCP-Protocol-Version: 2025-06-18\r\n"


def normal_call(host, port, timeout):
    """One normal MCP call. Returns (ok, detail, seconds)."""
    t0 = time.time()
    try:
        body = PING
        req = urllib.request.Request("http://%s:%d/mcp" % (host, port), data=body,
                                     method="POST",
                                     headers={"Content-Type": "application/json"})
        opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
        with opener.open(req, timeout=timeout) as r:
            r.read()
        return True, "HTTP %d" % r.status, time.time() - t0
    except urllib.error.HTTPError as e:
        return False, "HTTP %d %s" % (e.code, e.reason), time.time() - t0
    except Exception as exc:  # noqa: BLE001
        return False, type(exc).__name__ + ": " + str(exc)[:90], time.time() - t0


def phase_a(host, port, count, timeout):
    print("=== phase A: hold %d idle connections ===" % count)
    held = []
    try:
        for i in range(count):
            s = socket.create_connection((host, port), timeout=5)
            held.append(s)
        print("  opened %d idle connections" % len(held))
        ok, detail, dt = normal_call(host, port, timeout)
        print("  normal call during idle sockets -> %s (%s) in %.1fs" %
              ("OK" if ok else "FAILED", detail, dt))
        return ok
    finally:
        for s in held:
            try:
                s.close()
            except OSError:
                pass


def phase_b(host, port, timeout):
    print("=== phase B: partial body (Content-Length lies) ===")
    s = socket.create_connection((host, port), timeout=5)
    try:
        s.sendall(b"POST /mcp HTTP/1.1\r\nHost: %s\r\nContent-Length: 100000\r\n%s\r\n"
                  % (host.encode(), HDRS))
        s.sendall(b'{"jsonrpc":"2.0",')     # promise 100000 bytes, send 15
        time.sleep(0.3)
        ok, detail, dt = normal_call(host, port, timeout)
        print("  normal call while a body is incomplete -> %s (%s) in %.1fs" %
              ("OK" if ok else "FAILED", detail, dt))
        return ok
    finally:
        try:
            s.close()
        except OSError:
            pass


def phase_c(host, port, timeout, wait):
    print("=== phase C: recovery ===")
    deadline = time.time() + wait
    while time.time() < deadline:
        ok, detail, dt = normal_call(host, port, timeout)
        print("  normal call -> %s (%s) in %.1fs" %
              ("OK" if ok else "FAILED", detail, dt))
        if ok:
            return True
        time.sleep(2)
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("host")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--idle", type=int, default=8)
    ap.add_argument("--timeout", type=float, default=20.0)
    ap.add_argument("--wait", type=float, default=40.0)
    args = ap.parse_args()

    print("=== HTTP robustness probe: %s:%d ===" % (args.host, args.port))
    ok0, detail0, dt0 = normal_call(args.host, args.port, args.timeout)
    print("baseline normal call -> %s (%s) in %.1fs" %
          ("OK" if ok0 else "FAILED", detail0, dt0))
    if not ok0:
        print("device is not answering at all -- is it powered / on the same subnet?")
        return 1

    results = {}
    results["A"] = phase_a(args.host, args.port, args.idle, args.timeout)
    results["B"] = phase_b(args.host, args.port, args.timeout)
    results["C"] = phase_c(args.host, args.port, args.timeout, args.wait)

    print("=== summary ===")
    for k, v in results.items():
        print("  phase %s: %s" % (k, "server stayed responsive" if v else "SERVER WENT DOWN"))
    return 0 if all(results.values()) else 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as exc:  # noqa: BLE001
        print("ERROR: %s" % exc)
        sys.exit(2)
