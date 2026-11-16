#!/usr/bin/env python3
"""Smoke test for bridge/sophon_bridge.py — speak MCP to it over stdio.

Runs the bridge as a child process and drives a real MCP handshake through
its stdin/stdout, exactly like WorkBuddy (or any stdio MCP client) would.

Checks
  T1  initialize           -> serverInfo present, protocolVersion echoed
  T2  tools/list           -> non-empty, contains the 8 verb tools
  T3  tools/call status    -> ok:true with structuredContent
  T4  tools/call draw      -> ok:true (display write; skip if no display)
  T5  unknown tool         -> JSON-RPC error -32602 (protocol level, not isError)

Usage:
    python bridge/test_bridge.py
    python bridge/test_bridge.py --endpoint http://172.17.21.231:8080/mcp
"""

import argparse
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
BRIDGE = os.path.join(HERE, "sophon_bridge.py")

EXPECTED_TOOLS = ["status", "ls", "read", "capture", "input", "update", "flash",
                  "draw"]

results = []


def check(tag, desc, got, want):
    ok = got == want
    results.append(ok)
    print("  [%s] %-4s %-52s got=%r want=%r"
          % ("PASS" if ok else "FAIL", tag, desc, got, want))
    return ok


class Bridge:
    def __init__(self, args):
        self.proc = subprocess.Popen(
            [sys.executable, "-u", BRIDGE] + args,
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL, text=True, encoding="utf-8")

    def send(self, payload):
        self.proc.stdin.write(json.dumps(payload) + "\n")
        self.proc.stdin.flush()

    def recv(self):
        line = self.proc.stdout.readline()
        if not line:
            return None
        return json.loads(line)

    def call(self, payload):
        self.send(payload)
        return self.recv()

    def close(self):
        try:
            self.proc.stdin.close()
            self.proc.wait(timeout=5)
        except Exception:
            self.proc.kill()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--endpoint", default=os.environ.get("SOPHON_ENDPOINT"))
    ap.add_argument("--name", default="sophon-dongle")
    args = ap.parse_args()

    extra = ["--name", args.name]
    if args.endpoint:
        extra += ["--endpoint", args.endpoint]

    b = Bridge(extra)
    try:
        # ---- T1 initialize
        r = b.call({"jsonrpc": "2.0", "id": 1, "method": "initialize",
                    "params": {"protocolVersion": "2025-06-18",
                               "capabilities": {},
                               "clientInfo": {"name": "bridge-test",
                                              "version": "0"}}})
        info = (r or {}).get("result", {}).get("serverInfo", {})
        check("T1a", "initialize -> serverInfo present", bool(info), True)
        check("T1b", "initialize -> protocolVersion echoed",
              (r or {}).get("result", {}).get("protocolVersion"), "2025-06-18")

        b.send({"jsonrpc": "2.0", "method": "notifications/initialized"})

        # ---- T2 tools/list
        r = b.call({"jsonrpc": "2.0", "id": 2, "method": "tools/list"})
        tools = (r or {}).get("result", {}).get("tools", [])
        names = sorted(t.get("name") for t in tools)
        check("T2a", "tools/list -> non-empty", len(tools) > 0, True)
        check("T2b", "tools/list -> exactly the 8 verb tools",
              names, sorted(EXPECTED_TOOLS))

        # ---- T3 status
        r = b.call({"jsonrpc": "2.0", "id": 3, "method": "tools/call",
                    "params": {"name": "status", "arguments": {}}})
        sc = (r or {}).get("result", {}).get("structuredContent", {})
        check("T3a", "status -> ok:true", sc.get("ok"), True)
        check("T3b", "status -> device name present", bool(sc.get("device")), True)

        # ---- T4 draw
        r = b.call({"jsonrpc": "2.0", "id": 4, "method": "tools/call",
                    "params": {"name": "draw",
                               "arguments": {"text": "WB BRIDGE\nOK"}}})
        sc = (r or {}).get("result", {}).get("structuredContent", {})
        check("T4", "draw -> ok:true", sc.get("ok"), True)

        # ---- T5 unknown tool -> protocol error
        r = b.call({"jsonrpc": "2.0", "id": 5, "method": "tools/call",
                    "params": {"name": "nope", "arguments": {}}})
        code = ((r or {}).get("error") or {}).get("code")
        check("T5", "unknown tool -> JSON-RPC -32602", code, -32602)
    finally:
        b.close()

    total, passed = len(results), sum(results)
    print("\n=== summary ===\n  %d checks, %d passed, %d failed"
          % (total, passed, total - passed))
    return 0 if passed == total else 1


if __name__ == "__main__":
    sys.exit(main())
