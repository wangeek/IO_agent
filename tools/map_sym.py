#!/usr/bin/env python3
"""Resolve an address to the symbol that contains it, using the IDF .map file.

Usage: python tools/map_sym.py 0x4037f0a4 [more addresses...]
       python tools/map_sym.py --range 0x4037e000 0x40380000
"""
import re
import sys

MAP = "firmware/build/sophon.map"

SYM = re.compile(r"^\s*(0x[0-9a-fA-F]+)\s+(\S+)")


def load():
    syms = []
    with open(MAP, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            m = SYM.match(line)
            if m:
                try:
                    syms.append((int(m.group(1), 16), m.group(2)))
                except ValueError:
                    pass
    syms.sort()
    return syms


def resolve(syms, addr):
    best = None
    for a, n in syms:
        if a <= addr:
            best = (a, n)
        else:
            break
    return best


def main():
    syms = load()
    args = sys.argv[1:]
    if args and args[0] == "--range":
        lo, hi = int(args[1], 16), int(args[2], 16)
        for a, n in syms:
            if lo <= a < hi:
                print("0x%08x  %s" % (a, n))
        return 0
    for a in args:
        addr = int(a, 16)
        hit = resolve(syms, addr)
        if hit:
            print("%s -> 0x%08x  %s   (+%d)" % (a, hit[0], hit[1], addr - hit[0]))
        else:
            print("%s -> (no symbol)" % a)
    return 0


if __name__ == "__main__":
    sys.exit(main())
