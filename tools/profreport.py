#!/usr/bin/env python3
"""Symbolize a sonix_player profile.out (src/system/core/profiler.c).

    profreport.py <profile.out> <sonix_player_debug> [top]

The report has [maps], [threads] and [samples] sections; each sample is
"<pc-hex> <tid>". The binary is the unstripped one from the same build
(sonix_player_debug), so the addresses match the running (stripped) player.
"""
import bisect
import re
import subprocess
import sys
from collections import Counter


def load_symbols(binary):
    out = subprocess.run(["nm", "-n", "--defined-only", binary], capture_output=True, text=True, check=True).stdout
    addrs, names = [], []
    for line in out.splitlines():
        parts = line.split(maxsplit=2)
        if len(parts) < 3:
            continue
        try:
            addr = int(parts[0], 16)
        except ValueError:
            continue
        addrs.append(addr)
        names.append(parts[2])
    return addrs, names


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    report, binary = sys.argv[1], sys.argv[2]
    top = int(sys.argv[3]) if len(sys.argv) > 3 else 25

    section = None
    threads = {}
    samples = []
    maps = []
    for line in open(report):
        line = line.rstrip("\n")
        if line.startswith("["):
            section = line.strip("[]")
            continue
        if section == "threads":
            parts = line.split(maxsplit=1)
            if len(parts) == 2:
                threads[parts[0]] = parts[1]
        elif section == "samples":
            parts = line.split()
            if len(parts) == 2:
                samples.append((int(parts[0], 16), parts[1]))
        elif section == "maps":
            m = re.match(r"^([0-9a-f]+)-([0-9a-f]+) (\S+) \S+ \S+ \S+\s*(.*)$", line)
            if m:
                maps.append((int(m.group(1), 16), int(m.group(2), 16), m.group(4).strip()))

    if not samples:
        print("no samples")
        return 1

    addrs, names = load_symbols(binary)

    def sym(pc):
        i = bisect.bisect_right(addrs, pc) - 1
        if i < 0:
            return None
        name = names[i]
        off = pc - addrs[i]
        return f"{name}+{off:#x}" if off else name

    total = len(samples)
    by_func = Counter()
    by_thread = Counter()
    unknown = 0
    for pc, tid in samples:
        name = sym(pc)
        if name is None:
            unknown += 1
            name = f"?{pc:#x}"
        by_func[name] += 1
        by_thread[threads.get(tid, tid)] += 1

    print(f"{total} samples ({unknown} outside the symbol table)\n")
    print("=== by thread ===")
    for name, n in by_thread.most_common():
        print(f"{100.0 * n / total:5.1f}% {n:6d}  {name}")
    print("\n=== by function ===")
    for name, n in by_func.most_common(top):
        print(f"{100.0 * n / total:5.1f}% {n:6d}  {name}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
