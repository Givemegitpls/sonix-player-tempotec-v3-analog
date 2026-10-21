#!/usr/bin/env python3
"""Make fb0/blank a real panel power cycle on the TempoTec V3 Analog.

The stock jz-fb driver's `jzfb_blank` only fiddles with the LCDC/SLCDC control
registers: POWERDOWN clears LCDC_CTRL.ENA and SLCDC_CTRL.DMA_EN, UNBLANK sets
ENA back but leaves DMA_EN cleared, so an unblanked panel sits armed and shows
its last frame forever ("frozen glass").  The rails and the panel's own init
table are only ever touched by the PM suspend/resume path.

This rewrites `jzfb_blank` in place so the blank node does the same power cycle
that `jzfb_resume` does (which is verified working on the device):

  POWERDOWN (any non-zero level)
      clear LCDC_CTRL.ENA and SLCDC_CTRL.DMA_EN, then lcd_power_off()
  UNBLANK (level 0)
      lcd_power_on() -> jzfb_set_par() (panel init table + DMA_EN) -> jzfb_enable()

Everything is done inside the function's own bytes, so the decompressed image
does not grow.  The stock kernel image (`kernel.bin`, base virtual address
0x80010000, file offset = virt - 0x80010000) is patched, gzip'd exactly like the
stock uImage and wrapped in a rebuilt uImage header.

Usage:
    patch_kernel.py [STOCK_KERNEL_BIN] [STOCK_UIMAGE] [OUT_UIMAGE]

Defaults are the old working layout -- two directories above the tree that held the
sources, `firmware/tt/kernel.bin` and `firmware/tt/iso/UIMAGE.BIN` -- and do not
resolve in this repository. Pass the two stock files (and the output, if wanted)
explicitly; see README.md here.
"""

from __future__ import annotations

import gzip
import hashlib
import struct
import sys
from pathlib import Path

KERNEL_VADDR = 0x80010000
UIMAGE_HDR = 64

# md5 of the stock kernel.bin this patch is written against.
STOCK_KERNEL_MD5 = "aaa60f4fc2b4d592f92bc5a405e10695"

# --- symbolic encoders -------------------------------------------------------


def _r(op: int, rs: int, rt: int, rd: int, shamt: int, funct: int) -> int:
    return (op << 26) | (rs << 21) | (rt << 16) | (rd << 11) | (shamt << 6) | funct


def _i(op: int, rs: int, rt: int, imm: int) -> int:
    return (op << 26) | (rs << 21) | (rt << 16) | (imm & 0xFFFF)


def lw(rt: int, off: int, rs: int) -> int:
    return _i(0x23, rs, rt, off)


def sw(rt: int, off: int, rs: int) -> int:
    return _i(0x2B, rs, rt, off)


def addiu(rt: int, rs: int, imm: int) -> int:
    return _i(0x09, rs, rt, imm)


def and_(rd: int, rs: int, rt: int) -> int:
    return _r(0x00, rs, rt, rd, 0, 0x24)


def jal(vaddr: int) -> int:
    # target = (PC+4 & 0xF0000000) | (imm<<2); all kernel text is 0x8000_0000+.
    return (0x03 << 26) | ((vaddr >> 2) & 0x03FFFFFF)


def beq(rs: int, rt: int, off: int) -> int:
    return _i(0x04, rs, rt, off)


def bne(rs: int, rt: int, off: int) -> int:
    return _i(0x05, rs, rt, off)


NOP = 0
ZERO, A0, V0, V1, S0 = 0, 4, 2, 3, 16

# Register/field offsets in the driver's private struct (`info->par`).
PAR_FB_INFO = 0x54  # struct jzfb -> fb_info  (jzfb_resume passes this to set_par)
PAR_REGS = 0x60  # struct jzfb -> register base
LCDC_CTRL = 48  # register offset of LCDC_CTRL
SLCDC_CTRL = 164  # register offset of SLCDC_CTRL
FB_ENABLED = 0  # struct jzfb -> "enabled/blank" flag

# Function addresses (virtual), from the stock image / the device's kallsyms.
JZFB_BLANK = 0x80203F94
LCDC_POWER_OFF = 0x8001A038  # board pdata+0x6c
LCDC_POWER_ON = 0x8001A1B0  # board pdata+0x68
JZFB_SET_PAR = 0x80205AE8
JZFB_ENABLE = 0x80204794
# goodix touch: lcd_power_off tail-calls goodix_ts_close *after* it has cut
# LDO7/LDO8, which power the touch controller.  goodix_ts_close() takes no
# argument (it reads the global `ts` at 0x8072f518) and returns early when
# ts->0xe0 is already set, so calling it once while the rails are still on
# makes the copy inside lcd_power_off a no-op -- instead of an i2c access to a
# dead controller that spins past the 3 s watchdog.
GOODIX_TS_CLOSE = 0x80363BFC

# Byte offsets inside kernel.bin (virt - 0x80010000).
O_PROLOGUE_BNEZ = 0x1F3FA8  # bnez a0, powerdown  -> retarget at the cave
O_UNBLANK = 0x1F3FB0  # 9-word UNBLANK block
O_CAVE = 0x1F3FD4  # 13-word POWERDOWN block (reuses the freed UNBLANK tail)
O_EPILOGUE = 0x1F4028


def _b_off(src: int, target: int) -> int:
    return (target - (src + 4)) // 4


def build_patch() -> list[tuple[int, int]]:
    """Return (file_offset, new_word) pairs."""
    p: list[tuple[int, int]] = []

    def put(off: int, word: int) -> None:
        p.append((off, word))

    # 1. prologue: POWERDOWN now jumps at the cave instead of the old inline block.
    put(
        O_PROLOGUE_BNEZ,
        bne(A0, ZERO, _b_off(O_PROLOGUE_BNEZ, O_CAVE + KERNEL_VADDR)),
    )

    # 2. UNBLANK: the resume sequence.  a0 = info = *(info->par + 0x54) is loaded
    #    in each jal delay slot (s0 = info->par survives the calls).
    unblank = [
        jal(LCDC_POWER_ON),
        lw(A0, PAR_FB_INFO, S0),
        jal(JZFB_SET_PAR),
        lw(A0, PAR_FB_INFO, S0),
        jal(JZFB_ENABLE),
        lw(A0, PAR_FB_INFO, S0),
        addiu(V0, ZERO, 1),
        beq(ZERO, ZERO, _b_off(O_UNBLANK + 7 * 4, O_EPILOGUE + KERNEL_VADDR)),
        sw(V0, FB_ENABLED, S0),
    ]
    for i, w in enumerate(unblank):
        put(O_UNBLANK + i * 4, w)

    # 3. POWERDOWN: put the touch to sleep first (rails still on), then clear
    #    ENA/DMA_EN and cut the rails.  lcd_power_off's own goodix_ts_close then
    #    sees ts->0xe0 set and does no i2c.
    cave = [
        jal(GOODIX_TS_CLOSE),  # close the touch while its rails are up
        NOP,  # delay slot
        lw(V0, PAR_REGS, S0),
        addiu(V1, ZERO, -9),  # ~0x8  -> clears LCDC_CTRL.ENA
        lw(A0, LCDC_CTRL, V0),
        and_(A0, A0, V1),
        sw(A0, LCDC_CTRL, V0),
        addiu(V1, ZERO, -2),  # ~0x1  -> clears SLCDC_CTRL.DMA_EN
        lw(A0, SLCDC_CTRL, V0),
        and_(A0, A0, V1),
        sw(A0, SLCDC_CTRL, V0),
        jal(LCDC_POWER_OFF),
        sw(ZERO, FB_ENABLED, S0),  # delay slot: info->par->blank = 0
        beq(ZERO, ZERO, _b_off(O_CAVE + 13 * 4, O_EPILOGUE + KERNEL_VADDR)),
        NOP,
    ]
    for i, w in enumerate(cave):
        put(O_CAVE + i * 4, w)

    return p


def apply_patch(img: bytearray) -> None:
    for off, new in build_patch():
        struct.pack_into("<I", img, off, new)


def crc32(data: bytes) -> int:
    import zlib

    return zlib.crc32(data) & 0xFFFFFFFF


def build_uimage(stock_uimage: bytes, payload: bytes) -> bytes:
    """Rebuild the uImage with the same header fields, new size/dcrc/hcrc."""
    hdr = bytearray(stock_uimage[:UIMAGE_HDR])
    if struct.unpack(">I", hdr[0:4])[0] != 0x27051956:
        raise SystemExit("stock UIMAGE.BIN: bad uImage magic")
    struct.pack_into(">I", hdr, 12, len(payload))  # ih_size
    struct.pack_into(">I", hdr, 24, crc32(payload))  # ih_dcrc
    struct.pack_into(">I", hdr, 4, 0)  # ih_hcrc = 0 while hashing
    struct.pack_into(">I", hdr, 4, crc32(bytes(hdr)))
    return bytes(hdr) + payload


def main() -> int:
    here = Path(__file__).resolve().parent
    root = here.parents[1]  # the repository root
    stock_kernel = Path(sys.argv[1]) if len(sys.argv) > 1 else root / ".." / ".." / "firmware/tt/kernel.bin"
    stock_uimage = Path(sys.argv[2]) if len(sys.argv) > 2 else root / ".." / ".." / "firmware/tt/iso/UIMAGE.BIN"
    out = Path(sys.argv[3]) if len(sys.argv) > 3 else here / "UIMAGE.BIN"

    stock_uimage = stock_uimage.resolve()
    img = bytearray(stock_kernel.resolve().read_bytes())
    uimage = stock_uimage.read_bytes()

    # Guard: only the stock kernel this patch was derived from is accepted.
    got = hashlib.md5(img).hexdigest()
    if got != STOCK_KERNEL_MD5:
        raise SystemExit(f"kernel.bin md5 {got}, expected the stock {STOCK_KERNEL_MD5}")

    # The raw kernel is exactly the decompressed uImage payload.
    if gzip.decompress(uimage[UIMAGE_HDR:]) != bytes(img):
        raise SystemExit("kernel.bin is not the decompressed payload of that UIMAGE.BIN")

    print(f"    stock kernel : {len(img)} bytes, md5 {hashlib.md5(img).hexdigest()}")
    apply_patch(img)
    print(f"    patched kernel: {len(img)} bytes, md5 {hashlib.md5(img).hexdigest()}")

    payload = gzip.compress(bytes(img), compresslevel=9, mtime=0)
    # Match the stock uImage's gzip framing: OS byte 3 (Unix), XFL 2 (best).
    payload = payload[:9] + b"\x03" + payload[10:]
    print(f"    payload      : {len(payload)} bytes (stock {len(uimage) - UIMAGE_HDR})")

    out.write_bytes(build_uimage(uimage, payload))
    print(f"    {out}: {out.stat().st_size} bytes, md5 {hashlib.md5(out.read_bytes()).hexdigest()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
