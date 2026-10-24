#!/usr/bin/env python3
"""Find an IO_Agent dongle on the LAN and print its MCP endpoint.

The dongle broadcasts itself over mDNS as "_ioagent._tcp.local"; the instance
name is the device name (e.g. "ioagent-lab._ioagent._tcp.local") and the SRV
record points at "<device-name>.local" with the MCP port.

Two things this script exists to get right (both bite in the field):

  1. The host OS usually CANNOT resolve ".local" names (Windows'
     Resolve-DnsName / ping fail on them). So we speak mDNS directly.
  2. We must JOIN the multicast group 224.0.0.251. Binding UDP port 5353 is
     not the same as joining -- without an IGMP membership report, an AP with
     IGMP snooping drops every multicast frame addressed to us.

Exit codes:
    0  device found (endpoint printed)
    1  not found within the wait window
    2  socket / usage error

Usage:
    python discover.py                       # print endpoint
    python discover.py --json                # machine readable
    python discover.py --name ioagent-lab    # pick a device by name
    python discover.py --wait 30             # keep retrying for 30 s
"""

import argparse
import json
import socket
import struct
import sys
import time

MDNS_ADDR = "224.0.0.251"
MDNS_PORT = 5353
DEFAULT_SERVICE = "_ioagent._tcp.local"
DEFAULT_NAME = "ioagent-lab"

TYPES = {1: "A", 12: "PTR", 16: "TXT", 28: "AAAA", 33: "SRV", 255: "ANY"}


# ------------------------------------------------------------------ DNS codec

def encode_name(name):
    out = bytearray()
    for label in name.rstrip(".").split("."):
        raw = label.encode("ascii")
        out.append(len(raw))
        out += raw
    out.append(0)
    return bytes(out)


def parse_name(data, offset):
    labels = []
    jumped = False
    end = offset
    hops = 0
    while True:
        if offset >= len(data):
            raise ValueError("name overruns message")
        length = data[offset]
        if length == 0:
            offset += 1
            if not jumped:
                end = offset
            break
        if length & 0xC0 == 0xC0:
            ptr = ((length & 0x3F) << 8) | data[offset + 1]
            offset += 2
            if not jumped:
                end = offset
            offset = ptr
            jumped = True
            hops += 1
            if hops > 8:
                raise ValueError("compression loop")
            continue
        offset += 1
        labels.append(data[offset:offset + length].decode("ascii", "replace"))
        offset += length
        if not jumped:
            end = offset
    return ".".join(labels), end


def decode_rdata(rtype, data, offset, rdlength):
    raw = data[offset:offset + rdlength]
    if rtype == 1 and rdlength == 4:
        return socket.inet_ntoa(raw)
    if rtype == 12:
        name, _ = parse_name(data, offset)
        return name
    if rtype == 33 and rdlength >= 7:
        prio, weight, port = struct.unpack("!HHH", raw[:6])
        target, _ = parse_name(data, offset + 6)
        return "%d %d %d %s" % (prio, weight, port, target)
    if rtype == 16:
        items = []
        pos = 0
        while pos < len(raw):
            ln = raw[pos]
            items.append(raw[pos + 1:pos + 1 + ln].decode("utf-8", "replace"))
            pos += 1 + ln
        return " | ".join(items) if items else ""
    if rtype == 28 and rdlength == 16:
        return socket.inet_ntop(socket.AF_INET6, raw)
    return raw.hex()


def build_query(qname, qtype):
    header = struct.pack("!HHHHHH", 0, 0, 1, 0, 0, 0)
    return header + encode_name(qname) + struct.pack("!HH", qtype, 1)


def parse_response(data):
    if len(data) < 12:
        return []
    _, flags, qd, an, ns, ar = struct.unpack("!HHHHHH", data[:12])
    offset = 12
    for _ in range(qd):
        _, offset = parse_name(data, offset)
        offset += 4
    records = []
    for _ in range(an + ns + ar):
        if offset >= len(data):
            break
        name, offset = parse_name(data, offset)
        if offset + 10 > len(data):
            break
        rtype, rclass, ttl, rdlength = struct.unpack("!HHIH", data[offset:offset + 10])
        offset += 10
        if offset + rdlength > len(data):
            break
        records.append({
            "name": name,
            "type": TYPES.get(rtype, str(rtype)),
            "ttl": ttl,
            "rdata": decode_rdata(rtype, data, offset, rdlength),
        })
        offset += rdlength
    return records


# ------------------------------------------------------------------ transport

def local_ips():
    """Local IPv4 addresses, best guess first (routable ones)."""
    out = []
    try:
        host = socket.gethostname()
        for info in socket.getaddrinfo(host, None, socket.AF_INET):
            if info[4][0] not in out:
                out.append(info[4][0])
    except OSError:
        pass
    return out


def open_sockets(log):
    mcast = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    mcast.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    mcast.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, struct.pack("B", 255))
    mcast.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_LOOP, 1)
    mcast.bind(("", MDNS_PORT))
    mcast.settimeout(0.25)

    group = socket.inet_aton(MDNS_ADDR)
    for ip in local_ips():
        try:
            mcast.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP,
                             group + socket.inet_aton(ip))
            log("joined %s on %s" % (MDNS_ADDR, ip))
            break
        except OSError as exc:
            log("join %s on %s failed: %s" % (MDNS_ADDR, ip, exc))

    unicast = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    unicast.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, struct.pack("B", 255))
    unicast.bind(("", 0))
    unicast.settimeout(0.25)
    return mcast, unicast


def query_once(device, service, timeout, log):
    queries = [
        ("%s.local" % device, 1),
        ("%s.local" % device, 255),
        (service, 12),
        (service, 33),
        (service, 255),
    ]
    mcast, unicast = open_sockets(log)
    socks = [mcast, unicast]
    try:
        for qname, qtype in queries:
            try:
                mcast.sendto(build_query(qname, qtype), (MDNS_ADDR, MDNS_PORT))
            except OSError as exc:
                log("send %s failed: %s" % (qname, exc))
        seen = []
        keys = set()
        deadline = time.time() + timeout
        while time.time() < deadline:
            for sock in socks:
                try:
                    data, _src = sock.recvfrom(9000)
                except socket.timeout:
                    continue
                except OSError:
                    continue
                for rec in parse_response(data):
                    key = (rec["name"], rec["type"], rec["rdata"])
                    if key in keys:
                        continue
                    keys.add(key)
                    seen.append(rec)
            time.sleep(0.01)
    finally:
        for s in socks:
            s.close()
    return seen, resolve(seen, device, service)


def resolve(records, device, service):
    """Records -> (address, port, host, instance) or None."""
    instance = None
    for rec in records:
        if rec["type"] == "PTR" and rec["name"].startswith(service.split(".")[0]):
            instance = rec["rdata"]
            break
    if instance is None:
        # Some responders (lwIP's included) skip PTR and answer the
        # service-type query with the instance SRV instead.
        for rec in records:
            if rec["type"] == "SRV" and rec["name"].endswith("." + service):
                instance = rec["name"]
                break
    if instance is None:
        return None

    host = None
    port = None
    for rec in records:
        if rec["type"] == "SRV" and rec["name"] == instance:
            parts = rec["rdata"].split()
            port = int(parts[2])
            host = parts[3]
            break
    if host is None:
        return None

    address = None
    for rec in records:
        if rec["type"] == "A" and rec["name"] == host:
            address = rec["rdata"]
            break
    if address is None:
        return None

    return {"device": device, "instance": instance, "host": host,
            "address": address, "port": port,
            "url": "http://%s:%d/mcp" % (address, port)}


# ------------------------------------------------------------------ main

def main():
    ap = argparse.ArgumentParser(description="Find the IO_Agent device via mDNS")
    ap.add_argument("--name", default=DEFAULT_NAME, help="device name (NVS 'name')")
    ap.add_argument("--service", default=DEFAULT_SERVICE, help="DNS-SD service type")
    ap.add_argument("--timeout", type=float, default=4.0, help="per-round listen time")
    ap.add_argument("--wait", type=float, default=0.0, help="retry for N seconds")
    ap.add_argument("--json", action="store_true", help="print one JSON object only")
    args = ap.parse_args()

    if args.json:
        def log(_m):
            pass
    else:
        def log(m):
            print(m, file=sys.stderr)

    deadline = time.time() + (args.wait if args.wait > 0 else 0)
    records = []
    found = None
    while True:
        try:
            records, found = query_once(args.name, args.service, args.timeout, log)
        except OSError as exc:
            print("SOCKET_ERROR: %s" % exc, file=sys.stderr)
            return 2
        if found or time.time() >= deadline:
            break
        time.sleep(1.0)

    if args.json:
        out = {"found": bool(found)}
        if found:
            out.update(found)
        out["records"] = [
            {"name": r["name"], "type": r["type"], "ttl": r["ttl"], "rdata": r["rdata"]}
            for r in records
            if r["name"].startswith(args.name) or r["name"].endswith("." + args.service)
        ]
        print(json.dumps(out, ensure_ascii=False))
        return 0 if found else 1

    if not found:
        print("device '%s' not found on the LAN (service %s)" % (args.name, args.service),
              file=sys.stderr)
        print("check: is the dongle powered? is it on the same subnet? "
              "is the mDNS responder up (device/info.led == 0)?", file=sys.stderr)
        return 1
    print(found["url"])
    return 0


if __name__ == "__main__":
    sys.exit(main())
