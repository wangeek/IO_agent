#!/usr/bin/env python3
"""Acceptance probe for the renamed MCP contract (2026-10-24).

The contract this checks:

  N1  tools/list returns exactly the seven verb tools
      (status / ls / read / capture / input / update / flash)
  N2  no old tool name survives in the list
  N3  every tool has a title and an outputSchema
  N4  unknown tool                                  -> JSON-RPC error -32602
  N5  tools/call "status"                          -> 200, ok=true
  N6  tools/call "ls"                              -> 200, ok=true, files[] present
  N7  tools/call "read" with a missing name        -> isError:true
  N8  tools/call "read" with a name that is not
      on the volume                                 -> isError:true
  N9  tools/call "capture"                         -> 200 (ok true or isError)
  N10 tools/call "input" with a bogus action       -> isError:true
  N11 tools/call "input" with no action            -> isError:true
  N12 GET /mcp                                     -> 405 + Allow: POST
  N13 GET /files/<name> for a file that is on the
      volume                                       -> 200 with the raw bytes
  N14 GET /files/<name> for a missing file         -> 404
  N15 a tool result carries structuredContent
  N16 a failing tool result carries isError:true
      (the old contract only set it on some tools)
  N19 every "type" in every input/output schema is a legal JSON Schema type
      (a human sentence in "type" makes strict MCP endpoints reject the whole
      request -- deterministically, every turn, forever)

Every check is read-only: no tool that injects into the target machine is
called with an action that touches anything. "input" is only called with
deliberately invalid arguments, which the device rejects before touching
the HID link.

Usage:
    python tools/contract_probe.py 172.17.21.225
    python tools/contract_probe.py 172.17.21.225 --name sophon-lab

Exit codes: 0 = all rules pass, 1 = at least one rule failed, 2 = IO error
"""

import argparse
import json
import socket
import sys
import urllib.error
import urllib.request

TIMEOUT = 10.0

OLD_TOOL_NAMES = [
    "device/info", "device_info", "act", "observe", "watcher/status",
    "watcher_status", "deploy_payload", "output", "write", "type", "cat",
    "list", "cap",
]

NEW_TOOL_NAMES = ["status", "ls", "read", "capture", "input", "update", "flash"]


def raw_post(ip, port, path, body, headers, host=None):
    url = "http://%s:%d%s" % (ip, port, path)
    data = body if isinstance(body, bytes) else body.encode("utf-8")
    req = urllib.request.Request(url, data=data, method="POST")
    for k, v in headers.items():
        req.add_header(k, v)
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    try:
        with opener.open(req, timeout=TIMEOUT) as r:
            return r.status, r.read().decode("utf-8", "replace"), dict(r.headers)
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode("utf-8", "replace"), dict(e.headers)
    except (urllib.error.URLError, socket.timeout, OSError) as e:
        return 0, "IOERR: %s" % e, {}


def raw_get(ip, port, path, host=None):
    if host is not None:
        head = "GET %s HTTP/1.1\r\n" % path
        head += "Host: %s\r\n" % host
        head += "Connection: close\r\n\r\n"
        try:
            s = socket.create_connection((ip, port), timeout=TIMEOUT)
            s.sendall(head.encode("ascii"))
            status, body, hdrs = _read_http_response(s, TIMEOUT)
            s.close()
            return status, body, hdrs
        except OSError as e:
            return 0, "IOERR: %s" % e, {}
    url = "http://%s:%d%s" % (ip, port, path)
    req = urllib.request.Request(url, method="GET")
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    try:
        with opener.open(req, timeout=TIMEOUT) as r:
            return r.status, r.read().decode("utf-8", "replace"), dict(r.headers)
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode("utf-8", "replace"), dict(e.headers)
    except (urllib.error.URLError, socket.timeout, OSError) as e:
        return 0, "IOERR: %s" % e, {}


def _read_http_response(sock, timeout):
    """Parse one HTTP response: headers then Content-Length bytes.

    Do NOT read until EOF -- the esp_http_server keeps the connection alive
    regardless of the client's "Connection: close".
    """
    sock.settimeout(timeout)
    buf = b""
    while b"\r\n\r\n" not in buf:
        chunk = sock.recv(4096)
        if not chunk:
            break
        buf += chunk
    if b"\r\n\r\n" not in buf:
        return 0, buf.decode("utf-8", "replace"), {}
    head, _, rest = buf.partition(b"\r\n\r\n")
    lines = head.decode("utf-8", "replace").split("\r\n")
    status = 0
    hdrs = {}
    for i, line in enumerate(lines):
        if i == 0:
            try:
                status = int(line.split()[1])
            except (IndexError, ValueError):
                status = 0
        elif ":" in line:
            k, _, v = line.partition(":")
            hdrs[k.strip().lower()] = v.strip()
    try:
        clen = int(hdrs.get("content-length", "0") or 0)
    except ValueError:
        clen = 0
    body = rest
    while len(body) < clen:
        try:
            chunk = sock.recv(4096)
        except socket.timeout:
            break
        if not chunk:
            break
        body += chunk
    return status, body.decode("utf-8", "replace"), hdrs


def rpc(method, params=None, request_id=1):
    body = {"jsonrpc": "2.0", "id": request_id, "method": method}
    if params is not None:
        body["params"] = params
    return json.dumps(body)


def call_tool(ip, port, name, arguments=None, request_id=1):
    """Call one tool and return the parsed JSON-RPC envelope."""
    params = {"name": name}
    if arguments is not None:
        params["arguments"] = arguments
    st, body, _ = raw_post(ip, port, "/mcp",
                            rpc("tools/call", params, request_id),
                            {"Content-Type": "application/json"})
    try:
        return st, json.loads(body)
    except ValueError:
        return st, {"_raw": body}


RESULTS = []


def check(rule, desc, got, want):
    ok = (got == want)
    RESULTS.append((rule, desc, got, want, ok))
    print("  [%s] %-5s %-54s got=%-14s want=%s" % (
        "PASS" if ok else "FAIL", rule, desc, str(got)[:14], want))
    return ok


def check_true(rule, desc, got, want_bool):
    ok = (bool(got) == want_bool)
    RESULTS.append((rule, desc, got, want_bool, ok))
    print("  [%s] %-5s %-54s got=%-14s want=%s" % (
        "PASS" if ok else "FAIL", rule, desc, str(got)[:14], want_bool))
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ip")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--name", default="sophon-lab", help="device name in NVS")
    args = ap.parse_args()

    ip, port = args.ip, args.port
    JSON = {"Content-Type": "application/json"}

    print("=== MCP contract probe (renamed tools): %s:%d (device %s) ===" % (
        ip, port, args.name))

    # ---- N1/N2/N3: the tool list ----
    st, body, _ = raw_post(ip, port, "/mcp", rpc("tools/list"), JSON)
    tools = []
    if st == 200:
        try:
            tools = json.loads(body)["result"]["tools"]
        except (ValueError, KeyError):
            tools = []
    names = [t.get("name") for t in tools]
    check("N1", "tools/list returns exactly the seven verb tools",
          sorted(names), sorted(NEW_TOOL_NAMES))
    stale = [n for n in OLD_TOOL_NAMES if n in names]
    check("N2", "no old tool name survives", stale, [])
    missing_title = [n for n, t in zip(names, tools) if not t.get("title")]
    check("N3a", "every tool has a title", missing_title, [])
    missing_schema = [n for n, t in zip(names, tools) if not t.get("outputSchema")]
    check("N3b", "every tool has an outputSchema", missing_schema, [])

    # ---- N19: every "type" in every schema must be a legal JSON Schema type ----
    # 2026-10-25: the input tool's `modifiers` was declared as
    #   {"type": "array of ctrl/shift/alt/win"}   <- a human sentence, not a type
    # Strict MCP endpoints (the zhipu Anthropic-compatible layer) validate the
    # whole schema and reject the ENTIRE request on an unknown type. It is a
    # deterministic parameter error, not a transient failure: the failed turn
    # is not recorded, the tool catalogue is rebuilt unchanged, so every
    # following turn re-sends the same broken schema and fails again. One
    # illegal type in one tool takes the whole session down.
    JSON_TYPES = {"string", "number", "integer", "boolean", "array", "object", "null"}
    illegal = []

    def scan_type(node, path):
        if isinstance(node, dict):
            tv = node.get("type")
            if isinstance(tv, str) and tv not in JSON_TYPES:
                illegal.append("%s: type=%r" % (path, tv))
            for k, v in node.items():
                scan_type(v, "%s.%s" % (path, k))
        elif isinstance(node, list):
            for i, v in enumerate(node):
                scan_type(v, "%s[%d]" % (path, i))

    for t in tools:
        scan_type(t.get("inputSchema", {}), "inputSchema of '%s'" % t.get("name"))
        scan_type(t.get("outputSchema", {}), "outputSchema of '%s'" % t.get("name"))
    check("N19", "every schema 'type' is a legal JSON Schema type", illegal, [])

    # ---- N4: unknown tool -> -32602 ----
    st, env = call_tool(ip, port, "definitely_not_a_tool")
    code = env.get("error", {}).get("code") if isinstance(env, dict) else None
    check("N4", "unknown tool -> JSON-RPC error -32602", code, -32602)

    # ---- N5: status ----
    st, env = call_tool(ip, port, "status", {}, 2)
    res = env.get("result", {}) if isinstance(env, dict) else {}
    check("N5a", "status -> HTTP 200", st, 200)
    check("N5b", "status -> ok:true", res.get("structuredContent", {}).get("ok"), True)
    check_true("N5c", "status -> watcher state present",
               "watcher" in res.get("structuredContent", {}), True)

    # ---- N6: ls ----
    st, env = call_tool(ip, port, "ls", {}, 3)
    res = env.get("result", {}) if isinstance(env, dict) else {}
    sc = res.get("structuredContent", {})
    check("N6a", "ls -> ok:true", sc.get("ok"), True)
    check_true("N6b", "ls -> files[] array present", isinstance(sc.get("files"), list), True)
    ls_files = sc.get("files") or []
    first = ls_files[0] if ls_files else {}
    check_true("N6c", "ls entry has name/size/mtime",
               all(k in first for k in ("name", "size", "mtime")), True)

    # ---- N7/N8: read failures must set isError ----
    st, env = call_tool(ip, port, "read", {}, 4)
    res = env.get("result", {}) if isinstance(env, dict) else {}
    check("N7", "read without a name -> isError:true", res.get("isError"), True)

    st, env = call_tool(ip, port, "read", {"name": "NOSUCH.TXT"}, 5)
    res = env.get("result", {}) if isinstance(env, dict) else {}
    check("N8", "read of a missing file -> isError:true", res.get("isError"), True)

    # N8b: read a file that IS on the volume.
    # This is the path that used to crash the device (Guru Meditation /
    # LoadProhibited, EXCVADDR=0x45) when observe read an existing file:
    # a 21-byte file and a freshly written one both hung the device.
    # Reading an existing file must return content and leave the device alive.
    existing = None
    for e in (ls_files or []):
        if e.get("name") and e.get("size"):
            existing = e
            break
    if existing:
        st, env = call_tool(ip, port, "read", {"name": existing["name"]}, 11)
        res = env.get("result", {}) if isinstance(env, dict) else {}
        sc8 = res.get("structuredContent", {})
        check("N8b", "read of an existing file -> ok:true", sc8.get("ok"), True)
        check_true("N8c", "read of an existing file -> bytes returned",
                   isinstance(sc8.get("bytes"), int) and sc8.get("bytes", 0) > 0, True)
    else:
        check("N8b", "read of an existing file -> ok:true", "no-file-to-test", True)

    # ---- N18: a file over the inline threshold must come back as a URL ----
    # Big content must not be inlined into the JSON-RPC response: the
    # device's HTTP stack is a single task with a limited send buffer, and
    # an oversized response stalls the whole MCP channel.
    big = None
    for e in (ls_files or []):
        if e.get("name") and e.get("size", 0) > 64 * 1024:
            big = e
            break
    if big:
        st, env = call_tool(ip, port, "read", {"name": big["name"]}, 18)
        res = env.get("result", {}) if isinstance(env, dict) else {}
        sc18 = res.get("structuredContent", {})
        check("N18a", "read of a big file -> ok:true", sc18.get("ok"), True)
        check_true("N18b", "read of a big file -> url instead of inline bytes",
                   bool(sc18.get("url")) and not sc18.get("bytes"), True)
    else:
        check("N18a", "read of a big file -> ok:true", "no-big-file-to-test", True)

    # ---- N9: capture (read-only) ----
    st, env = call_tool(ip, port, "capture", {}, 6)
    res = env.get("result", {}) if isinstance(env, dict) else {}
    sc = res.get("structuredContent", {})
    if sc.get("ok"):
        check("N9", "capture -> ok:true (a frame is on the volume)", sc.get("ok"), True)
    else:
        # no watcher running: the device must say so *and* flag it as an error
        check("N9", "capture with no frame -> isError:true", res.get("isError"), True)

    # ---- N10/N11: input rejects bad arguments before touching the HID link ----
    st, env = call_tool(ip, port, "input", {"action": "no_such_action"}, 7)
    res = env.get("result", {}) if isinstance(env, dict) else {}
    check("N10", "input with a bogus action -> isError:true", res.get("isError"), True)

    st, env = call_tool(ip, port, "input", {}, 8)
    res = env.get("result", {}) if isinstance(env, dict) else {}
    check("N11", "input without an action -> isError:true", res.get("isError"), True)

    # ---- N17: flash rejects a bad url BEFORE touching the network ----
    # (no OTA is started: the argument check happens first)
    st, env = call_tool(ip, port, "flash", {}, 17)
    res = env.get("result", {}) if isinstance(env, dict) else {}
    check("N17", "flash without a url -> isError:true", res.get("isError"), True)

    # ---- N12: GET /mcp -> 405 ----
    st, _, hdrs = raw_get(ip, port, "/mcp")
    check("N12a", "GET /mcp -> 405 (not 404)", st, 405)
    # header names are case-insensitive; the socket path lowercases them,
    # the urllib path keeps the server's casing -- normalise both.
    allow = ""
    for k, v in hdrs.items():
        if k.lower() == "allow":
            allow = v
    check("N12b", "GET /mcp -> Allow: POST", allow, "POST")

    # ---- N13/N14: /files/<name> ----
    # pick a file that ls reported, so this does not depend on a frame existing
    victim = None
    for e in (ls_files or []):
        if e.get("name"):
            victim = e["name"]
            break
    if victim:
        st, body, _ = raw_get(ip, port, "/files/%s" % victim)
        check("N13", "GET /files/<name on the volume> -> 200", st, 200)
    else:
        check("N13", "GET /files/<name on the volume> -> 200", "no-file-to-test", 200)
    st, _, _ = raw_get(ip, port, "/files/NOSUCH.TXT")
    check("N14", "GET /files/<missing name> -> 404", st, 404)

    # ---- N15: structuredContent present on a successful call ----
    st, env = call_tool(ip, port, "ls", {}, 9)
    res = env.get("result", {}) if isinstance(env, dict) else {}
    check_true("N15", "successful tool result carries structuredContent",
               isinstance(res.get("structuredContent"), dict), True)

    # ---- N16: a failing tool result carries isError ----
    st, env = call_tool(ip, port, "read", {"name": "NOSUCH.TXT"}, 10)
    res = env.get("result", {}) if isinstance(env, dict) else {}
    check("N16", "failing tool result carries isError:true", res.get("isError"), True)

    failed = [r for r in RESULTS if not r[4]]
    print("=== summary ===")
    print("  %d checks, %d passed, %d failed" % (
        len(RESULTS), len(RESULTS) - len(failed), len(failed)))
    for r in failed:
        print("  FAIL %s: %s (got %s want %s)" % (r[0], r[1], r[2], r[3]))
    return 0 if not failed else 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as exc:  # noqa: BLE001
        print("ERROR: %s" % exc)
        sys.exit(2)
