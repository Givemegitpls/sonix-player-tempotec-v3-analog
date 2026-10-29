#!/usr/bin/env python3
"""TempoTec V3 Analog boot diagnostics: decode what the device reported.

Two inputs are possible from the owner:

  * a blink *count* -- the odometer, one blink per 10 initcalls:
        ./v3_report.py count 21          -> the boot passed ~210 initcalls
  * the panic sequence -- a header of 3 blinks, then three digit groups where a group of
    N blinks is the digit N-1:
        ./v3_report.py groups 3 3 7 7    -> header + digits 2, 6, 6 -> ordinal 266

The ordinal is looked up in the initcall list of the built vmlinux (initcalls_by_addr.txt is
written by the nm/awk one-liner in NOTES.md; entries are in link order, which is the order they
run in).
"""
import sys
from pathlib import Path

LIST = Path(__file__).with_name("initcalls_by_addr.txt")


def load():
    rows = []
    for line in LIST.read_text().splitlines():
        addr, name = line.split()
        name = name.replace("__initcall_", "")
        while name and name[-1] in "0123456789st":
            name = name[:-1]
        rows.append((int(addr, 16), name))
    rows.sort()
    return rows


def name_of(ordinal):
    rows = load()
    if not 1 <= ordinal <= len(rows):
        return f"(out of range: the build has {len(rows)} initcalls)"
    return rows[ordinal - 1][1]


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return
    mode = sys.argv[1]
    if mode == "count":
        n = int(sys.argv[2])
        print(f"odometer: {n} blinks = {n * 10} initcalls returned")
        for k in range(n, n + 2):
            print(f"  the culprit is within ordinals {k * 10 + 1}..{(k + 1) * 10}")
        return
    if mode == "groups":
        g = [int(x) for x in sys.argv[2:]]
        header, digits = g[0], g[1:]
        if header != 3:
            print(f"warning: the header should be 3 blinks, got {header}")
        decoded = "".join(str(d - 1) for d in digits)
        ordinal = int(decoded)
        print(f"blink groups {g} -> digits {decoded} -> ordinal {ordinal}")
        print(f"  last initcall started: {name_of(ordinal)}")
        if ordinal < 267:
            print(f"  next one (never started): {name_of(ordinal + 1)}")
        return
    print(__doc__)


if __name__ == "__main__":
    main()
