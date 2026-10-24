#!/usr/bin/env python3
"""Call the IO_Agent MCP server (or fetch a blob) from the host.

Why this script exists instead of "just use curl":

  * The host may have an HTTP proxy configured (http_proxy / HTTP_PROXY).
    A LAN address must NOT go through it -- the proxy either refuses the
    connection or answers for it. This script bypasses proxies explicitly.
  * The server is strict about the request shape: POST /mcp with
    Content-Type: application/json, body <= 128 KB, Host/Origin bound to the
    device. Getting any of those wrong looks like a mysterious 4xx.
  * JSON-RPC calls need a hand-built envelope; doing that inline in a shell
    is where quoting bugs live.

Usage:
    python mcp_call.py --endpoint http://172.17.21.225:8080/mcp --info
    python mcp_call.py --endpoint http://172.17.21.225:8080/mcp --list
    python mcp_call.py --endpoint http://172.17.21.225:8080/mcp \
        --tool device/info
    python mcp_call.py --endpoint http://172.17.21.225:8080/mcp \
        --tool act --args '{"action":"tap","key":"ENTER"}'
    python mcp_call.py --endpoint http://172.17.21.225:8080/mcp \
        --method tools/list
    python mcp_call.py --fetch http://172.17.21.225:8080/blob/b0 --out frame.jpg

    --args / --params accept a JSON string, or @path to read JSON from a file.

Exit codes:
    0  call succeeded (HTTP 2xx)
    1  server answered with an error status (printed to stderr)
    2  transport / usage error
"""

import argparse
import json
import os
import sys
import urllib.error
import urllib.request

TIMEOUT = 30.0
PROTOCOL_VERSION = "2025-06-18"
MAX_BODY = 128 * 1024


def read_json_arg(value):
    """--args / --params: inline JSON, or @file."""
    if value is None:
        return None
    if value.startswith("@"):
        path = value[1:]
        try:
            with open(path, "r", encoding="utf-8") as f:
                return json.load(f)
        except (OSError, ValueError) as exc:
            raise SystemExit("cannot read %s: %s" % (path, exc))
    try:
        return json.loads(value)
    except ValueError as exc:
        raise SystemExit("not valid JSON: %s" % exc)


def opener():
    """An opener that never uses a proxy.

    The dongle is on the LAN; a configured proxy would break the call.
    """
    for var in ("http_proxy", "https_proxy", "HTTP_PROXY", "HTTPS_PROXY",
                "ALL_PROXY", "all_proxy"):
        os.environ.pop(var, None)
    return urllib.request.build_opener(urllib.request.ProxyHandler({}))


def rpc(endpoint, method, params=None, request_id=1):
    body = {"jsonrpc": "2.0", "id": request_id, "method": method}
    if params is not None:
        body["params"] = params
    raw = json.dumps(body).encode("utf-8")
    if len(raw) > MAX_BODY:
        raise SystemExit("request body %d bytes exceeds the device limit (%d)"
                         % (len(raw), MAX_BODY))

    req = urllib.request.Request(endpoint, data=raw, method="POST")
    req.add_header("Content-Type", "application/json")
    req.add_header("MCP-Protocol-Version", PROTOCOL_VERSION)
    req.add_header("Accept", "application/json, text/event-stream")

    try:
        with opener().open(req, timeout=TIMEOUT) as resp:
            text = resp.read().decode("utf-8", "replace")
            return resp.status, text
    except urllib.error.HTTPError as exc:
        return exc.code, exc.read().decode("utf-8", "replace")
    except (urllib.error.URLError, OSError) as exc:
        raise SystemExit("transport error: %s" % exc)


def call_tool(endpoint, tool, args):
    params = {"name": tool}
    if args is not None:
        params["arguments"] = args
    return rpc(endpoint, "tools/call", params)


def fetch(url, out_path):
    req = urllib.request.Request(url, method="GET")
    try:
        with opener().open(req, timeout=TIMEOUT) as resp:
            data = resp.read()
    except urllib.error.HTTPError as exc:
        raise SystemExit("HTTP %d fetching %s: %s"
                         % (exc.code, url, exc.read().decode("utf-8", "replace")[:200]))
    except (urllib.error.URLError, OSError) as exc:
        raise SystemExit("transport error: %s" % exc)
    if out_path:
        with open(out_path, "wb") as f:
            f.write(data)
        print("saved %d bytes -> %s" % (len(data), out_path))
    else:
        sys.stdout.buffer.write(data)
    return 0


def main():
    ap = argparse.ArgumentParser(description="IO_Agent MCP client")
    ap.add_argument("--endpoint", help="MCP endpoint, e.g. http://172.17.21.225:8080/mcp")
    ap.add_argument("--tool", help="call a tool by name (tools/call)")
    ap.add_argument("--args", help="tool arguments: JSON string or @file")
    ap.add_argument("--method", help="raw JSON-RPC method, e.g. tools/list")
    ap.add_argument("--params", help="JSON-RPC params: JSON string or @file")
    ap.add_argument("--info", action="store_true", help="shortcut for --tool device/info")
    ap.add_argument("--list", action="store_true", help="shortcut for --method tools/list")
    ap.add_argument("--fetch", help="fetch a blob URL instead of calling MCP")
    ap.add_argument("--out", help="where --fetch writes the bytes")
    ap.add_argument("--raw", action="store_true", help="print the raw HTTP body")
    args = ap.parse_args()

    if args.fetch:
        return fetch(args.fetch, args.out)

    if not args.endpoint:
        raise SystemExit("--endpoint is required (run discover.py to get it)")

    if args.info:
        status, body = call_tool(args.endpoint, "device/info", None)
    elif args.tool:
        status, body = call_tool(args.endpoint, args.tool, read_json_arg(args.args))
    elif args.method:
        status, body = rpc(args.endpoint, args.method, read_json_arg(args.params))
    elif args.list:
        status, body = rpc(args.endpoint, "tools/list")
    else:
        raise SystemExit("nothing to do: pass --info / --list / --tool / --method / --fetch")

    if args.raw:
        print(body)
        return 0 if 200 <= status < 300 else 1

    if not (200 <= status < 300):
        print("HTTP %d" % status, file=sys.stderr)
        print(body[:600], file=sys.stderr)
        print("--- likely causes ---", file=sys.stderr)
        print("  415: Content-Type must be application/json", file=sys.stderr)
        print("  413: request body over 128 KB", file=sys.stderr)
        print("  403: Host/Origin not bound to the device (use the exact IP or <name>.local)",
              file=sys.stderr)
        print("  400: unsupported MCP-Protocol-Version", file=sys.stderr)
        return 1

    try:
        parsed = json.loads(body)
    except ValueError:
        print(body)
        return 0
    print(json.dumps(parsed, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
