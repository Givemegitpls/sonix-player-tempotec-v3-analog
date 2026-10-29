#!/usr/bin/env python3
"""Decode the symbol table out of the stock kernel image.

`firmware/tt/kernel.bin` is a raw MIPS kernel with `CONFIG_KALLSYMS` and an uncompressed
symbol table, so the vendor's function names can be recovered without the device (the
recipe and the offsets are in `docs/KERNEL.md` / `.opencode/NOTES.md`):

    kallsyms_addresses    file 0x5739f0   (21443 words, 4 bytes each)
    num_syms              0x53c3          at file 0x588900
    kallsyms_names        file 0x58890c
    kallsyms_token_table  file 0x5c3080
    kallsyms_token_index  file 0x5c33c0   (256 x u16)

Names are token-compressed: a length byte (0x80 in it means a two-byte length), then that
many one-byte indexes into the token table. The first character of a decoded name is the
symbol type. There are four empty name entries in front of the stream, so address `k`
belongs to name `k + 4`.

Usage:
    python3 decode_kallsyms.py [kernel.bin] [--grep PATTERN] [--out FILE]
"""

from __future__ import annotations

import argparse
import re
import struct
from pathlib import Path

ADDRESSES = 0x5739F0
NUM_SYMS = 0x588900
NAMES = 0x58890C
TOKEN_TABLE = 0x5C3080
TOKEN_INDEX = 0x5C33C0
NAME_SKIP = 4

BASE = 0x80010000


def decode(data: bytes) -> list[tuple[int, str]]:
    n = struct.unpack_from("<I", data, NUM_SYMS)[0]
    addrs = struct.unpack_from(f"<{n}I", data, ADDRESSES)
    # the token table runs up to the token index, and the index entries are offsets into
    # that region, not ordinal numbers of tokens
    region = data[TOKEN_TABLE:TOKEN_INDEX]
    index = struct.unpack_from("<256H", data, TOKEN_INDEX)

    def token(b: int) -> bytes:
        start = index[b]
        return region[start : region.index(b"\0", start)]

    out: list[tuple[int, str]] = []
    pos = NAMES
    for i in range(n + NAME_SKIP):
        length = data[pos]
        pos += 1
        if length & 0x80:
            length = (length & 0x7F) | (data[pos] << 7)
            pos += 1
        raw = data[pos : pos + length]
        pos += length
        if i < NAME_SKIP:
            continue
        name = b"".join(token(b) for b in raw).decode("latin-1")
        out.append((addrs[i - NAME_SKIP], name))
    return out


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("kernel", nargs="?", default="firmware/tt/kernel.bin")
    ap.add_argument("--grep", default=None, help="only print names matching this regex")
    ap.add_argument("--out", default=None, help="write the whole list here")
    args = ap.parse_args()

    data = Path(args.kernel).read_bytes()
    symbols = decode(data)
    print(f"{len(symbols)} symbols decoded from {args.kernel}")

    known = {"jzfb_blank": 0x80203F94, "lcd_power_on": 0x8001A1B0}
    for name, addr in known.items():
        hit = [hex(a) for a, s in symbols if s[1:] == name]
        print(f"  check {name}: {hit} (expected {hex(addr)})")

    if args.out:
        with Path(args.out).open("w") as f:
            for addr, name in symbols:
                f.write(f"{addr:08x} {name}\n")
        print(f"  wrote {args.out}")

    if args.grep:
        pattern = re.compile(args.grep)
        for addr, name in symbols:
            if pattern.search(name):
                print(f"{addr:08x} {name}")


if __name__ == "__main__":
    main()
