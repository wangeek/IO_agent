#!/usr/bin/env python3
"""Find a Sophon device on the LAN via mDNS (no third-party dependencies).

Why a bespoke resolver instead of the OS one:

  * Windows does not resolve arbitrary ".local" names (Resolve-DnsName /
    ping fail), so an agent running on Windows gets nothing from the system
    resolver. macOS/Linux do, but only for the exact hostname -- the agent
    still has to know it, and the device name is configurable in NVS.
  * lwIP's mDNS responder does not implement PTR (reverse) records at all, so
    a generic zeroconf browser (python-zeroconf, Bonjour) cannot enumerate
    the service type. It does answer the service-type query with the
    instance SRV record, which is what this tool uses.

So the discovery path implemented here is:

    query _ioagent._tcp.local (SRV/ANY)  ->  instance SRV  ->  host:port
    query <host>.local   (A)             ->  IPv4 address

Usage:
    python tools/discover.py                      # verbose, single shot
    python tools/discover.py --json               # machine readable, single shot
    python tools/discover.py --json --wait 20     # keep retrying for 20 s
    python tools/discover.py --name sophon-lab   # override device name

Exit codes:
    0  device found (address + port resolved)
    1  not found within the wait window
    2  usage / socket error
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
DEFAULT_NAME = "sophon-lab"

TYPES = {
    1: "A",
    12: "PTR",
    16: "TXT",
    28: "AAAA",
    33: "SRV",
    255: "ANY",
}


# ---------------------------------------------------------------- DNS codec

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
    question = encode_name(qname) + struct.pack("!HH", qtype, 1)
    return header + question


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
            "cache_flush": bool(rclass & 0x8000),
            "ttl": ttl,
            "rdata": decode_rdata(rtype, data, offset, rdlength),
        })
        offset += rdlength
    return records


# ---------------------------------------------------------------- transport

def _candidate_local_ips(target):
    """Local IPv4 addresses worth joining the group on.

    Priority: the source address the OS would use to reach the target (that is
    the interface the mDNS traffic will actually travel on), then every other
    local IPv4 as a fallback.
    """
    out = []
    if target:
        try:
            probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            probe.connect((target, 9))
            out.append(probe.getsockname()[0])
            probe.close()
        except OSError:
            pass
    try:
        host = socket.gethostname()
        for info in socket.getaddrinfo(host, None, socket.AF_INET):
            if info[4][0] not in out:
                out.append(info[4][0])
    except OSError:
        pass
    return out


def open_sockets(target, log):
    """Open the two sockets an mDNS querier needs.

    * port 5353 + multicast group membership -> receives multicast answers
    * ephemeral port                          -> receives legacy unicast answers

    Joining the group is NOT optional: binding the port is not the same as
    joining, and an AP with IGMP snooping will not deliver 224.0.0.251 to a
    host that never sent a membership report.
    """
    mcast = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    mcast.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    mcast.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, struct.pack("B", 255))
    mcast.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_LOOP, 1)
    mcast.bind(("", MDNS_PORT))
    mcast.settimeout(0.25)

    group = socket.inet_aton(MDNS_ADDR)
    joined = False
    for local_ip in _candidate_local_ips(target):
        try:
            mcast.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP,
                             group + socket.inet_aton(local_ip))
            log("    joined %s on %s" % (MDNS_ADDR, local_ip))
            joined = True
            break
        except OSError as exc:
            log("    join %s on %s failed: %s" % (MDNS_ADDR, local_ip, exc))
    if not joined:
        log("    WARNING: could not join %s -- multicast answers may be "
            "dropped by the AP" % MDNS_ADDR)

    unicast = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    unicast.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, struct.pack("B", 255))
    unicast.bind(("", 0))
    unicast.settimeout(0.25)
    return mcast, unicast


def query_once(device, service, timeout, target, log):
    """One discovery round. Returns (records, found_dict_or_None)."""
    queries = [
        ("%s.local" % device, 1, "A"),
        ("%s.local" % device, 255, "ANY"),
        (service, 12, "PTR"),
        (service, 33, "SRV"),
        (service, 16, "TXT"),
        (service, 255, "ANY"),
    ]
    dest = (target, MDNS_PORT) if target else (MDNS_ADDR, MDNS_PORT)

    mcast, unicast = open_sockets(target, log)
    socks = [mcast, unicast]
    try:
        log("=== sending mDNS queries to %s:%d ===" % (dest[0], dest[1]))
        log("    listening on 0.0.0.0:%d (multicast) and %d (unicast)" % (
            MDNS_PORT, unicast.getsockname()[1]))
        for qname, qtype, label in queries:
            try:
                mcast.sendto(build_query(qname, qtype), dest)
                log("  -> %-28s %s" % (qname, label))
            except OSError as exc:
                log("  -> %-28s %s  SEND_FAILED %s" % (qname, label, exc))

        seen = []
        seen_keys = set()
        deadline = time.time() + timeout
        while time.time() < deadline:
            got = False
            for sock in socks:
                try:
                    data, src = sock.recvfrom(9000)
                except socket.timeout:
                    continue
                except OSError:
                    continue
                got = True
                log("  [rx %s:%d %d bytes]" % (src[0], src[1], len(data)))
                for rec in parse_response(data):
                    key = (rec["name"], rec["type"], rec["rdata"])
                    if key in seen_keys:
                        continue
                    seen_keys.add(key)
                    seen.append(rec)
            if not got:
                time.sleep(0.01)
    finally:
        for s in socks:
            s.close()

    return seen, resolve(seen, device, service)


def resolve(records, device, service):
    """Turn raw records into (address, port, host) or None."""
    instance = None
    for rec in records:
        if rec["type"] == "PTR" and rec["name"].startswith(service.split(".")[0]):
            instance = rec["rdata"]
            break
    if instance is None:
        # lwIP's responder has no PTR support: it answers the service-type
        # query with the instance SRV record instead.
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

    return {
        "device": device,
        "instance": instance,
        "host": host,
        "address": address,
        "port": port,
        "url": "http://%s:%d/mcp" % (address, port),
    }


# ---------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser(description="Find the Sophon device via mDNS")
    ap.add_argument("--name", default=DEFAULT_NAME, help="device name in NVS")
    ap.add_argument("--service", default=DEFAULT_SERVICE, help="DNS-SD service type")
    ap.add_argument("--timeout", type=float, default=4.0, help="per-round listen time")
    ap.add_argument("--wait", type=float, default=0.0,
                    help="keep retrying for this many seconds (0 = single shot)")
    ap.add_argument("--target", default=None,
                    help="unicast query to this IP:5353 instead of the multicast group")
    ap.add_argument("--json", action="store_true", help="print one JSON object only")
    args = ap.parse_args()

    if args.json:
        def log(_msg):
            pass
    else:
        def log(msg):
            print(msg)

    deadline = time.time() + (args.wait if args.wait > 0 else 0)
    records = []
    found = None
    while True:
        try:
            records, found = query_once(args.name, args.service, args.timeout,
                                        args.target, log)
        except OSError as exc:
            if args.json:
                print(json.dumps({"found": False, "error": str(exc)}))
            else:
                print("SOCKET_ERROR: %s" % exc)
            return 2
        if found or time.time() >= deadline:
            break
        if not args.json:
            print("  ... not found yet, retrying")
        time.sleep(1.0)

    if args.json:
        out = {"found": bool(found)}
        if found:
            out.update(found)
        # Only keep records that belong to our device/service: the LAN usually
        # has plenty of unrelated mDNS chatter (printers, NAS, ...).
        mine = [r for r in records
                if r["name"].startswith(args.name) or r["name"].endswith("." + args.service)]
        out["records"] = [
            {"name": r["name"], "type": r["type"], "ttl": r["ttl"], "rdata": r["rdata"]}
            for r in mine
        ]
        print(json.dumps(out, ensure_ascii=False))
        return 0 if found else 1

    print("=== answers ===")
    if not records:
        print("  (no response at all)")
        return 1
    for rec in records:
        print("  %-40s %-5s ttl=%-5d %s" % (
            rec["name"], rec["type"], rec["ttl"], rec["rdata"]))
    if not found:
        print("=== summary ===")
        print("  device NOT resolved (service %s)" % args.service)
        return 1
    print("=== summary ===")
    print("  %s -> %s:%d" % (args.name, found["address"], found["port"]))
    print("  MCP endpoint: %s" % found["url"])
    return 0


if __name__ == "__main__":
    sys.exit(main())
