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

Five switches, each a few words in the decompressed image. `--poll` is the one that answers "the
player stopped seeing the card"; `--timeout-ms` is a fault injector, not a fix.

  --freq 24|48      the slot's `max_freq` in the board's platform data. 24 MHz
                    costs half the read throughput and halves the driver's
                    timing margin; the A/B of 2026-11-26 says it makes stalls
                    worse, so 48 (stock) is the default. (E1)
  --tick2-ms N      the second window of `jzmmc_request_timeout`: the delay it
                    re-arms and the number its first tick prints (both 3000).
                    A patient window means the driver's own timer never fails a
                    request the card was about to finish. (S2b)
  --retry-errno     complete a timed-out request with -EILSEQ instead of -EIO.
                    `mmc_blk_cmd_error()` answers ERR_RETRY for a CRC error and
                    `mmc_blk_issue_rw_rq()` re-sends the request up to five times
                    without needing a host reset -- but only if the card answers
                    the status command, so this is the riskier of the two. (S2a)
  --timeout-ms N    both windows of `jzmmc_request_timeout` set to N ms, the
                    arming in `jzmmc_request` included. This is not a fix: it is
                    a fault injector. With N=200 any pause of the card longer
                    than 0.4 s ends the request with -EIO, i.e. exactly the
                    chain a real stall produces ("Filesystem has been set
                    read-only"), reproducibly and on demand. Used to verify the
                    player's remount path on a live card. (I1)
  --poll            MMC_CAP_NEEDS_POLL in the board's `.capacity`: mmc_rescan()
                    then runs once a second and re-probes the card on its own.
                    This is the one that answers "the card is in, the player
                    says it is not": after a detect bounce the driver may power
                    the slot down and, with no poll, nothing ever powers it up
                    again. (P1)

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

# ...and its .capacity, which jzmmc_probe() ORs into mmc->caps:
# MMC_CAP_4_BIT_DATA | MMC_CAP_MMC_HIGHSPEED | MMC_CAP_SD_HIGHSPEED = 0x7.
# MMC_CAP_NEEDS_POLL is bit 5, so 0x27 makes mmc_rescan() run once a second and
# re-probe the card instead of waiting for a pin edge that never comes.
O_CAPACITY = 0x63E1EC
CAPACITY_STOCK = 0x7
CAPACITY_POLL = 0x27

# jzmmc_request_timeout (virt 0x80389444): the constant the first tick re-arms
# the timer with, and the same constant it prints in `timeout %dms`.
O_TICK1_PRINT_MS = 0x379498  # `li s3,3000` at virt 0x80389498
O_TICK1_REARM_MS = 0x37957C  # `li a0,3000` at virt 0x8038957c, argument of msecs_to_jiffies

# jzmmc_request (virt 0x80388bd8) arms the same timer when a request starts:
# `li a0,3000` at virt 0x80388f30, then `sw zero,2640(s0)` clears host->timeout_cnt
# (2640 = 0xa50) and mod_timer() is called on host+1004. Patching this one and
# the re-arm together sets the whole window the driver gives a request.
O_ARM_TIMEOUT_MS = 0x378F30

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


def build_patch(
    freq: int | None,
    tick2_ms: int | None,
    retry_errno: bool,
    timeout_ms: int | None = None,
    poll: bool = False,
) -> list[tuple[int, int, str]]:
    """Return (file_offset, new_word, description) triples."""
    p: list[tuple[int, int, str]] = []

    if freq is not None:
        p.append((O_MAX_FREQ, freq, f"max_freq {freq // 1_000_000} MHz"))

    if poll:
        p.append((O_CAPACITY, CAPACITY_POLL, "capacity + MMC_CAP_NEEDS_POLL"))

    if timeout_ms is not None:
        # Both windows, and the number the timer prints, so the log tells the
        # truth about the window that is running.
        p.append((O_ARM_TIMEOUT_MS, li(LI_A0_BASE, timeout_ms), f"window {timeout_ms} ms (arm)"))
        p.append((O_TICK1_REARM_MS, li(LI_A0_BASE, timeout_ms), f"window {timeout_ms} ms (re-arm)"))
        p.append((O_TICK1_PRINT_MS, li(LI_S3_BASE, timeout_ms), f"window {timeout_ms} ms (printed)"))

    if tick2_ms is not None:
        p.append((O_TICK1_REARM_MS, li(LI_A0_BASE, tick2_ms), f"second window {tick2_ms} ms (re-arm)"))
        p.append((O_TICK1_PRINT_MS, li(LI_S3_BASE, tick2_ms), f"second window {tick2_ms} ms (printed)"))

    if retry_errno:
        # -EILSEQ = -84. mmc_blk_cmd_error() maps it to ERR_RETRY, which
        # mmc_blk_issue_rw_rq() re-sends (up to five times) without a reset.
        p.append((O_DATA_ERRNO, li(LI_A0_BASE, -84), "data->error -EILSEQ"))
        p.append((O_CMD_ERRNO, li(LI_A0_BASE, -84), "cmd->error -EILSEQ"))

    return p


def apply_patch(img: bytearray, sites: list[tuple[int, int, str]]) -> list[str]:
    """Write the sites into the image; return the notes for the ones already right."""
    skipped: list[str] = []
    for off, new, what in sites:
        old = struct.unpack_from("<I", img, off)[0]
        if old == new:
            skipped.append(f"0x{off:06x} already holds 0x{new:08x}: {what}")
            continue
        struct.pack_into("<I", img, off, new)
    return skipped


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
    ap.add_argument(
        "--freq",
        type=int,
        choices=(24, 48),
        default=48,
        help="card bus clock in MHz (default 48, the stock value; 24 lost its A/B and is kept as a tool)",
    )
    ap.add_argument("--tick2-ms", type=int, default=None, help="second timeout window; omit to keep 3000")
    ap.add_argument("--retry-errno", action="store_true", help="time out with -EILSEQ so the block layer retries")
    ap.add_argument(
        "--timeout-ms",
        type=int,
        default=None,
        help="fault injector: both timeout windows and the printed value set to N ms (S2b is --tick2-ms)",
    )
    ap.add_argument(
        "--poll",
        action="store_true",
        help="set MMC_CAP_NEEDS_POLL: mmc_rescan() runs once a second and re-probes the card (P1)",
    )
    args = ap.parse_args()

    if args.timeout_ms is not None and args.tick2_ms is not None:
        raise SystemExit("--timeout-ms sets both windows; --tick2-ms would fight it")

    img = bytearray(Path(args.kernel).read_bytes())
    uimage = Path(args.uimage).read_bytes()

    got = hashlib.md5(img).hexdigest()
    if got != STOCK_KERNEL_MD5:
        raise SystemExit(f"kernel.bin md5 {got}, expected the stock {STOCK_KERNEL_MD5}")
    if gzip.decompress(uimage[UIMAGE_HDR:]) != bytes(img):
        raise SystemExit("kernel.bin is not the decompressed payload of that UIMAGE.BIN")

    freq = args.freq * 1_000_000
    sites = build_patch(freq, args.tick2_ms, args.retry_errno, args.timeout_ms, args.poll)
    if not sites:
        raise SystemExit("no switches given: nothing would change")

    print(f"    stock kernel  : {len(img)} bytes, md5 {got}")
    for off, new, what in sites:
        old = struct.unpack_from("<I", img, off)[0]
        mark = "same" if old == new else "    "
        print(f"    0x{off:06x}      : 0x{old:08x} -> 0x{new:08x}  {what}  {mark}")
    skipped = apply_patch(img, sites)
    if len(skipped) == len(sites):
        raise SystemExit("every site already holds its new value: nothing would change")
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
