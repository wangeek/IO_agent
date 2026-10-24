#!/usr/bin/env python3
"""Call the Sophon MCP server (or fetch a file) from the host.

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
    python mcp_call.py --endpoint http://172.17.21.225:8080/mcp --status
    python mcp_call.py --endpoint http://172.17.21.225:8080/mcp --list
    python mcp_call.py --endpoint http://172.17.21.225:8080/mcp \
        --tool status
    python mcp_call.py --endpoint http://172.17.21.225:8080/mcp \
        --tool input --args '{"action":"tap","key":"ENTER"}'
    python mcp_call.py --endpoint http://172.17.21.225:8080/mcp \
        --method tools/list
    python mcp_call.py --fetch http://172.17.21.225:8080/files/SNAP_P.JPG --out frame.jpg

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
import time
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
            with open(path, "r", encoding="utf-8-sig") as f:
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


def rpc(endpoint, method, params=None, request_id=1, timeout=TIMEOUT):
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
        with opener().open(req, timeout=timeout) as resp:
            text = resp.read().decode("utf-8", "replace")
            return resp.status, text
    except urllib.error.HTTPError as exc:
        return exc.code, exc.read().decode("utf-8", "replace")
    except (urllib.error.URLError, OSError) as exc:
        raise SystemExit("transport error: %s" % exc)


def call_tool(endpoint, tool, args, timeout=TIMEOUT):
    params = {"name": tool}
    if args is not None:
        params["arguments"] = args
    return rpc(endpoint, "tools/call", params, timeout=timeout)


def structured(envelope):
    """Pull the machine-readable payload out of a tool result."""
    if isinstance(envelope, str):
        try:
            envelope = json.loads(envelope)
        except ValueError:
            return {"raw": envelope}
    if isinstance(envelope, dict) and "error" in envelope:
        return envelope
    result = (envelope or {}).get("result", {})
    sc = result.get("structuredContent")
    if isinstance(sc, dict):
        return sc
    return result


def call_and_parse(endpoint, tool, args=None, timeout=TIMEOUT):
    """Call one tool and return its structured payload."""
    _, body = call_tool(endpoint, tool, args, timeout)
    return structured(body)


def wait_for(endpoint, condition, timeout=30.0, interval=0.5):
    """Poll the device until a condition holds. Returns (payload, waited_s, ok).

    ★ Why this lives on the CLIENT, not on the device ★
    The device runs one HTTP task. A device-side "wait" tool would have to
    hold that task for the whole wait, which would block every other
    request -- including the read-only ones -- for as long as the action
    runs. That is exactly the hole the device-side worker task was built to
    avoid. So the polling stays here: the device keeps serving, and the
    agent still sees ONE step instead of a dozen.

    Two conditions, mirroring the tool names:
      "idle"     -> status.busy becomes false (the action finished)
      "file:<N>" -> the named file shows up in ls (an external event)
    """
    deadline = time.time() + timeout
    started = time.time()
    payload = {}
    while True:
        if condition.startswith("file:"):
            name = condition.split(":", 1)[1].upper()
            payload = call_and_parse(endpoint, "ls")
            ok = any(e.get("name", "").upper() == name
                     for e in payload.get("files", []))
        else:
            payload = call_and_parse(endpoint, "status")
            ok = not payload.get("busy")
        if ok:
            return payload, time.time() - started, True
        if time.time() >= deadline:
            return payload, time.time() - started, False
        time.sleep(interval)


def fetch(url, out_path, timeout=TIMEOUT):
    req = urllib.request.Request(url, method="GET")
    try:
        with opener().open(req, timeout=timeout) as resp:
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
    ap = argparse.ArgumentParser(description="Sophon MCP client")
    ap.add_argument("--endpoint", help="MCP endpoint, e.g. http://172.17.21.225:8080/mcp")
    ap.add_argument("--tool", help="call a tool by name (tools/call)")
    ap.add_argument("--args", help="tool arguments: JSON string or @file")
    ap.add_argument("--method", help="raw JSON-RPC method, e.g. tools/list")
    ap.add_argument("--params", help="JSON-RPC params: JSON string or @file")
    ap.add_argument("--status", action="store_true", help="shortcut for --tool status")
    ap.add_argument("--list", action="store_true", help="shortcut for --method tools/list")
    ap.add_argument("--fetch", help="fetch a file URL instead of calling MCP")
    ap.add_argument("--out", help="where --fetch writes the bytes")
    ap.add_argument("--timeout", type=float, default=TIMEOUT,
                    help="seconds before giving up (default %d)" % int(TIMEOUT))
    ap.add_argument("--raw", action="store_true", help="print the raw HTTP body")
    ap.add_argument("--wait", action="store_true",
                    help="poll until no action is running (status.busy == false)")
    ap.add_argument("--wait-file", metavar="NAME",
                    help="poll until this 8.3 file name appears on the shared disk")
    ap.add_argument("--wait-timeout", type=float, default=30.0,
                    help="seconds to keep polling (default 30)")
    args = ap.parse_args()

    if args.fetch:
        return fetch(args.fetch, args.out, timeout=args.timeout)

    if not args.endpoint:
        raise SystemExit("--endpoint is required (run discover.py to get it)")

    if args.wait or args.wait_file:
        condition = ("file:%s" % args.wait_file) if args.wait_file else "idle"
        payload, waited, ok = wait_for(args.endpoint, condition, args.wait_timeout)
        out = dict(payload) if isinstance(payload, dict) else {}
        out["waited_s"] = round(waited, 2)
        out["timed_out"] = not ok
        print(json.dumps(out, ensure_ascii=False, indent=2))
        if not ok:
            print("timed out after %.1f s waiting for %s" % (waited, condition),
                  file=sys.stderr)
        return 0 if ok else 1

    if args.status:
        status, body = call_tool(args.endpoint, "status", None, args.timeout)
    elif args.tool:
        status, body = call_tool(args.endpoint, args.tool, read_json_arg(args.args),
                                 args.timeout)
    elif args.method:
        status, body = rpc(args.endpoint, args.method, read_json_arg(args.params),
                            timeout=args.timeout)
    elif args.list:
        status, body = rpc(args.endpoint, "tools/list", timeout=args.timeout)
    else:
        raise SystemExit("nothing to do: pass --status / --list / --tool / --method / --fetch")

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

    # A tool result carries the machine-readable payload in structuredContent.
    # Print that (not the whole envelope) so the output is directly usable,
    # and surface the image block separately -- it is base64 and would drown
    # the terminal.
    if isinstance(parsed, dict) and "result" in parsed:
        result = parsed["result"] or {}
        if isinstance(result, dict) and "structuredContent" in result:
            sc = result["structuredContent"]
            image = None
            for item in result.get("content", []) or []:
                if item.get("type") == "image":
                    image = item
                    break
            if image is not None:
                if args.out:
                    import base64
                    data = base64.b64decode(image.get("data", ""))
                    with open(args.out, "wb") as f:
                        f.write(data)
                    print("saved %d bytes (%s) -> %s" % (
                        len(data), image.get("mimeType", "?"), args.out))
                else:
                    print("image block: %d base64 chars (%s) -- pass --out to save it"
                          % (len(image.get("data", "")), image.get("mimeType", "?")),
                          file=sys.stderr)
            if result.get("isError"):
                print("(the tool reported an error -- isError: true)", file=sys.stderr)
            print(json.dumps(sc, ensure_ascii=False, indent=2))
            return 0
        if "error" in parsed:
            err = parsed["error"]
            print("JSON-RPC error %s: %s" % (err.get("code"), err.get("message")),
                  file=sys.stderr)
            print(json.dumps(parsed, ensure_ascii=False, indent=2))
            return 1

    print(json.dumps(parsed, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
