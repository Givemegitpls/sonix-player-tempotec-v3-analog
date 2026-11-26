#!/usr/bin/env python3
"""Card-stall patches for the stock 3.10.14 kernel of the TempoTec V3 Analog.

The stock `jzmmc_v12` driver gives a card three seconds, twice, and then fails the
request with -EIO. On this board the card is a large, nearly full SDXC and its
own programming pauses have been seen to outlast the first window; the failure
then takes the filesystem down with it (`Filesystem has been set read-only`),
the block layer cannot recover (`host->ops->hw_reset` is NULL, so
`mmc_blk_reset()` returns -EOPNOTSUPP), and nothing re-initialises the card. See
docs/KERNEL-MMC.md and .opencode/NOTES.md for the disassembly behind every offset
below.

Three independent switches, each a few words in the decompressed image:

  --freq 24|48      the slot's `max_freq` in the board's platform data. 24 MHz
                    costs half the read throughput and buys twice the timing
                    margin -- the cheap test for "the card is marginal at 48 MHz".
                    (E1)
  --tick2-ms N      the second window of `jzmmc_request_timeout`: the delay it
                    re-arms and the number its first tick prints (both 3000).
                    A patient window means the driver's own timer never fails a
                    request the card was about to finish. (S2b)
  --retry-errno     complete a timed-out request with -EILSEQ instead of -EIO.
                    `mmc_blk_cmd_error()` answers ERR_RETRY for a CRC error and
                    `mmc_blk_issue_rw_rq()` re-sends the request up to five times
                    without needing a host reset -- but only if the card answers
                    the status command, so this is the riskier of the two. (S2a)

The stock kernel image (base virtual address 0x80010000, so file offset =
virt - 0x80010000) is patched in place, gzip'd exactly like the stock uImage and
wrapped in a rebuilt header, the same way patch/v3-panel-blank does it.

Usage:
    patch_kernel.py STOCK_KERNEL_BIN STOCK_UIMAGE [OUT_UIMAGE] [switches]

Both stock files are yours to produce: take `UIMAGE.BIN` out of your own vendor
`v3_analog.upt` and gunzip its 64-byte-headered payload to get `kernel.bin`.

    mkdir -p /tmp/kv3 && cd /tmp/kv3
    7z x -y <vendor>.upt
    python3 -c 'import gzip; d=open("UIMAGE.BIN","rb").read(); open("kernel.bin","wb").write(gzip.decompress(d[64:]))'
    patch/v3-mmc/patch_kernel.py kernel.bin UIMAGE.BIN out/UIMAGE.BIN --freq 24
"""

from __future__ import annotations

import argparse
import gzip
import hashlib
import struct
import sys
import zlib
from pathlib import Path

KERNEL_VADDR = 0x80010000
UIMAGE_HDR = 64

# md5 of the stock kernel.bin every offset in this file was read off.
STOCK_KERNEL_MD5 = "aaa60f4fc2b4d592f92bc5a405e10695"

# --- the patch sites (file offsets in kernel.bin) ----------------------------

# `jzmmc_platform_data` of MMC0, the card's controller: .max_freq, "up to 48 MHz".
O_MAX_FREQ = 0x63E1F4
FREQ_48 = 48_000_000
FREQ_24 = 24_000_000

# jzmmc_request_timeout (virt 0x80389444): the constant the first tick re-arms
# the timer with, and the same constant it prints in `timeout %dms`.
O_TICK1_PRINT_MS = 0x379498  # `li s3,3000` at virt 0x80389498
O_TICK1_REARM_MS = 0x37957C  # `li a0,3000` at virt 0x8038957c, argument of msecs_to_jiffies

# The second tick completes the request: data->error and cmd->error are -EIO.
O_DATA_ERRNO = 0x379730
O_CMD_ERRNO = 0x37975C

LI_A0_BASE = 0x24040000
LI_S3_BASE = 0x24130000


def reg_i(op: int, rs: int, rt: int, imm: int) -> int:
    return (op << 26) | (rs << 21) | (rt << 16) | (imm & 0xFFFF)


def li(rt_base: int, imm: int) -> int:
    """`li rt, imm` as the assembler encodes a small signed constant: addiu rt,$0,imm."""
    return reg_i(0x09, 0, (rt_base >> 16) & 0x1F, imm)


def build_patch(freq: int | None, tick2_ms: int | None, retry_errno: bool) -> list[tuple[int, int, str]]:
    """Return (file_offset, new_word, description) triples."""
    p: list[tuple[int, int, str]] = []

    if freq is not None:
        p.append((O_MAX_FREQ, freq, f"max_freq {freq // 1_000_000} MHz"))

    if tick2_ms is not None:
        p.append((O_TICK1_REARM_MS, li(LI_A0_BASE, tick2_ms), f"second window {tick2_ms} ms (re-arm)"))
        p.append((O_TICK1_PRINT_MS, li(LI_S3_BASE, tick2_ms), f"second window {tick2_ms} ms (printed)"))

    if retry_errno:
        # -EILSEQ = -84. mmc_blk_cmd_error() maps it to ERR_RETRY, which
        # mmc_blk_issue_rw_rq() re-sends (up to five times) without a reset.
        p.append((O_DATA_ERRNO, li(LI_A0_BASE, -84), "data->error -EILSEQ"))
        p.append((O_CMD_ERRNO, li(LI_A0_BASE, -84), "cmd->error -EILSEQ"))

    return p


def apply_patch(img: bytearray, sites: list[tuple[int, int, str]]) -> None:
    for off, new, _ in sites:
        old = struct.unpack_from("<I", img, off)[0]
        if old == new:
            raise SystemExit(f"0x{off:x} already holds 0x{new:08x}: nothing to do?")
        struct.pack_into("<I", img, off, new)


def build_uimage(stock_uimage: bytes, payload: bytes) -> bytes:
    hdr = bytearray(stock_uimage[:UIMAGE_HDR])
    if struct.unpack(">I", hdr[0:4])[0] != 0x27051956:
        raise SystemExit("stock UIMAGE.BIN: bad uImage magic")
    struct.pack_into(">I", hdr, 12, len(payload))  # ih_size
    struct.pack_into(">I", hdr, 24, zlib.crc32(payload) & 0xFFFFFFFF)  # ih_dcrc
    struct.pack_into(">I", hdr, 4, 0)  # ih_hcrc while hashing
    struct.pack_into(">I", hdr, 4, zlib.crc32(bytes(hdr)) & 0xFFFFFFFF)
    return bytes(hdr) + payload


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("kernel", help="stock kernel.bin (the gunzipped uImage payload)")
    ap.add_argument("uimage", help="the stock UIMAGE.BIN the kernel came from")
    ap.add_argument("out", nargs="?", default="UIMAGE.BIN", help="output uImage")
    ap.add_argument("--freq", type=int, choices=(24, 48), default=24, help="card bus clock in MHz (default 24)")
    ap.add_argument("--tick2-ms", type=int, default=None, help="second timeout window; omit to keep 3000")
    ap.add_argument("--retry-errno", action="store_true", help="time out with -EILSEQ so the block layer retries")
    args = ap.parse_args()

    img = bytearray(Path(args.kernel).read_bytes())
    uimage = Path(args.uimage).read_bytes()

    got = hashlib.md5(img).hexdigest()
    if got != STOCK_KERNEL_MD5:
        raise SystemExit(f"kernel.bin md5 {got}, expected the stock {STOCK_KERNEL_MD5}")
    if gzip.decompress(uimage[UIMAGE_HDR:]) != bytes(img):
        raise SystemExit("kernel.bin is not the decompressed payload of that UIMAGE.BIN")

    freq = args.freq * 1_000_000
    sites = build_patch(freq, args.tick2_ms, args.retry_errno)
    if not sites:
        raise SystemExit("no switches given: nothing would change")

    print(f"    stock kernel  : {len(img)} bytes, md5 {got}")
    for off, new, what in sites:
        old = struct.unpack_from("<I", img, off)[0]
        print(f"    0x{off:06x}      : 0x{old:08x} -> 0x{new:08x}  {what}")
    apply_patch(img, sites)
    print(f"    patched kernel: {len(img)} bytes, md5 {hashlib.md5(img).hexdigest()}")

    payload = gzip.compress(bytes(img), compresslevel=9, mtime=0)
    payload = payload[:9] + b"\x03" + payload[10:]  # OS byte 3 (Unix), as the stock uImage has it
    print(f"    payload       : {len(payload)} bytes (stock {len(uimage) - UIMAGE_HDR})")

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(build_uimage(uimage, payload))
    print(f"    {out}: {out.stat().st_size} bytes, md5 {hashlib.md5(out.read_bytes()).hexdigest()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
