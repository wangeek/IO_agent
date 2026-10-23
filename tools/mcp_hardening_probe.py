#!/usr/bin/env python3
"""Acceptance probe for the MCP request hardening (P2/P4).

Checks, in order, each rule the device must enforce:

  R1  normal POST /mcp (application/json)            -> 200
  R2  wrong Content-Type                             -> 415
  R3  missing Content-Type                           -> 415
  R4  Host header that is not the device             -> 403
  R5  Host header with the mDNS name                 -> 200 (our own alias)
  R6  Origin header pointing elsewhere                -> 403
  R7  Origin header pointing at the device           -> 200
  R8  body larger than the cap                       -> 413
  R9  initialize with an older supported version     -> echoes that version
  R10 initialize with a bogus version                -> answers with our latest
  R11 non-initialize request with a bad
      MCP-Protocol-Version header                    -> 400
  R12 non-initialize request with a good
      MCP-Protocol-Version header                    -> 200
  R13 no Mcp-Session-Id in the response              -> stateless
  R14 notification                                   -> 202, empty body
  R15 GET /ping with a foreign Host                  -> 403
  R16 GET /ping normal                               -> 200

Usage:
    python tools/mcp_hardening_probe.py 172.17.21.225
    python tools/mcp_hardening_probe.py 172.17.21.225 --name ioagent-lab

Exit codes: 0 = all rules pass, 1 = at least one rule failed, 2 = IO error
"""

import argparse
import json
import socket
import sys
import urllib.error
import urllib.request

TIMEOUT = 10.0


def raw_post(ip, port, path, body, headers, host=None):
    """Send a request and return (status, body_text, headers) without raising."""
    url = "http://%s:%d%s" % (ip, port, path)
    data = body if isinstance(body, bytes) else body.encode("utf-8")
    req = urllib.request.Request(url, data=data, method="POST")
    for k, v in headers.items():
        req.add_header(k, v)
    if host is not None:
        # urllib sets Host itself; override it by talking to a socket below.
        return raw_post_socket(ip, port, path, data, headers, host)
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
    regardless of the client's "Connection: close", so reading to EOF just
    blocks until the socket timeout.
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


def raw_post_socket(ip, port, path, data, headers, host):
    """Same as raw_post but with a hand-written Host header."""
    head = "POST %s HTTP/1.1\r\n" % path
    head += "Host: %s\r\n" % host
    head += "Content-Length: %d\r\n" % len(data)
    head += "Connection: close\r\n"
    for k, v in headers.items():
        head += "%s: %s\r\n" % (k, v)
    head += "\r\n"
    try:
        s = socket.create_connection((ip, port), timeout=TIMEOUT)
        s.sendall(head.encode("ascii") + data)
        status, body, hdrs = _read_http_response(s, TIMEOUT)
        s.close()
        return status, body, hdrs
    except OSError as e:
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


def rpc(method, params=None, request_id=1):
    body = {"jsonrpc": "2.0", "id": request_id, "method": method}
    if params is not None:
        body["params"] = params
    return json.dumps(body)


RESULTS = []


def check(rule, desc, got, want):
    ok = (got == want)
    RESULTS.append((rule, desc, got, want, ok))
    print("  [%s] %-4s %-52s got=%-4s want=%s" % (
        "PASS" if ok else "FAIL", rule, desc, got, want))
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ip")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--name", default="ioagent-lab", help="device name in NVS")
    args = ap.parse_args()

    ip = args.ip
    port = args.port
    fqdn = "%s.local" % args.name
    JSON = {"Content-Type": "application/json"}

    print("=== MCP hardening probe: %s:%d (device %s) ===" % (ip, port, args.name))

    # R1 normal
    st, body, hdrs = raw_post(ip, port, "/mcp", rpc("ping"), JSON)
    check("R1", "normal POST application/json", st, 200)

    # R2 wrong content type
    st, _, _ = raw_post(ip, port, "/mcp", rpc("ping"), {"Content-Type": "text/plain"})
    check("R2", "wrong Content-Type -> 415", st, 415)

    # R3 missing content type
    st, _, _ = raw_post(ip, port, "/mcp", rpc("ping"), {})
    check("R3", "missing Content-Type -> 415", st, 415)

    # R4 foreign Host
    st, _, _ = raw_post(ip, port, "/mcp", rpc("ping"), JSON, host="evil.example.com")
    check("R4", "foreign Host -> 403", st, 403)

    # R5 our mDNS alias
    st, _, _ = raw_post(ip, port, "/mcp", rpc("ping"), JSON, host="%s:%d" % (fqdn, port))
    check("R5", "Host = our mDNS name -> 200", st, 200)

    # R6 cross origin
    st, _, _ = raw_post(ip, port, "/mcp", rpc("ping"),
                        dict(JSON, Origin="http://evil.example.com"))
    check("R6", "foreign Origin -> 403", st, 403)

    # R7 same origin
    st, _, _ = raw_post(ip, port, "/mcp", rpc("ping"),
                        dict(JSON, Origin="http://%s:%d" % (ip, port)))
    check("R7", "Origin = our own -> 200", st, 200)

    # R8 oversized body (cap is 128 KB -> send 200 KB)
    big = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "ping",
                      "params": {"pad": "A" * (200 * 1024)}})
    st, _, _ = raw_post(ip, port, "/mcp", big, JSON)
    check("R8", "body over cap -> 413", st, 413)

    # R9 negotiate an older supported version
    st, body, _ = raw_post(ip, port, "/mcp",
                            rpc("initialize", {"protocolVersion": "2024-11-05"}), JSON)
    ver = ""
    if st == 200:
        try:
            ver = json.loads(body)["result"]["protocolVersion"]
        except (ValueError, KeyError):
            ver = "<parse-fail>"
    check("R9", "initialize 2024-11-05 -> echo same version", ver, "2024-11-05")

    # R10 negotiate a bogus version
    st, body, _ = raw_post(ip, port, "/mcp",
                            rpc("initialize", {"protocolVersion": "1999-01-01"}), JSON)
    ver = ""
    if st == 200:
        try:
            ver = json.loads(body)["result"]["protocolVersion"]
        except (ValueError, KeyError):
            ver = "<parse-fail>"
    check("R10", "initialize bogus version -> our latest", ver, "2025-06-18")

    # R11 bad MCP-Protocol-Version header
    st, _, _ = raw_post(ip, port, "/mcp", rpc("ping"),
                        dict(JSON, **{"MCP-Protocol-Version": "1999-01-01"}))
    check("R11", "bad MCP-Protocol-Version header -> 400", st, 400)

    # R12 good MCP-Protocol-Version header
    st, _, _ = raw_post(ip, port, "/mcp", rpc("ping"),
                        dict(JSON, **{"MCP-Protocol-Version": "2025-06-18"}))
    check("R12", "good MCP-Protocol-Version header -> 200", st, 200)

    # R13 no fake session id
    st, body, hdrs = raw_post(ip, port, "/mcp", rpc("ping"), JSON)
    sid = ""
    for k, v in hdrs.items():
        if k.lower() == "mcp-session-id":
            sid = v
    check("R13", "no Mcp-Session-Id in response", sid, "")

    # R14 notification
    st, body, _ = raw_post(ip, port, "/mcp",
                            json.dumps({"jsonrpc": "2.0", "method": "notifications/initialized"}),
                            JSON)
    check("R14", "notification -> 202", st, 202)
    check("R14b", "notification -> empty body", len(body.strip()), 0)

    # R15 foreign Host on GET /ping
    st, _, _ = raw_get(ip, port, "/ping", host="evil.example.com")
    check("R15", "GET /ping with foreign Host -> 403", st, 403)

    # R16 normal GET /ping
    st, _, _ = raw_get(ip, port, "/ping")
    check("R16", "GET /ping normal -> 200", st, 200)

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
