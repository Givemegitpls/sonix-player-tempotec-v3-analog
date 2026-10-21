# V3 Analog: `fb0/blank` as a real panel power cycle

## Status: not shipped (unverified, and the blank path Oopses)

This patch is kept as a documented attempt, **not** as something the port ships. What was
learned, in order:

1. The first iteration (calling `lcd_power_off`/`lcd_power_on` from `jzfb_blank`) hung the
   device: `lcd_power_off`'s tail call is `goodix_ts_close`, which talks i2c to the touch
   controller *after* the rails that power it are off, so the CPU ended in `__delay` and the
   3 s watchdog reset the SoC (`CPU0 RESET ERROR PC:801E88C0`).
2. Calling `goodix_ts_close` first (rails still up) fixes that ordering -- but a live capture
   then showed the blank node Oopsing for **both** directions, with `epc == 0` and `ra == 0`,
   including immediately after `jzfb_set_par` in the UNBLANK branch. Both branches are code
   this patch adds, and both work in the PM path; the sysfs `blank` write is the one that runs
   under `console_lock`.
3. So driving the panel power sequence from `fb_blank` is not safe on this kernel as patched,
   and a proper fix (deferred work, outside `console_lock`) needs the kernel **source**, which
   HiBy never published: the device's kernel is their 3.10.14 build, and the X1000 tree we have
   is a mainline 5.0-rc1 bring-up skeleton with no display driver for this panel.

The image this repo builds therefore carries the **stock kernel**; the bluealsa fix (the
Bluetooth stutter) is independent of this and is shipped. To try the patch again, copy
`UIMAGE.BIN` next to `sonix-packer/kernel/` as the README below describes, and capture the
kernel log during the first blank (`kcap.sh` on the card does that).


Makes `/sys/class/graphics/fb0/blank` actually power the SLCD panel off and on
on the TempoTec V3 Analog (Ingenic X1000E, Linux 3.10.14), instead of only
toggling the LCDC/SLCDC control bits.  Delivered as a binary patch of the stock
kernel image; the artifact is a full, flashable `v3_analog.upt`.

Nothing here was flashed: the device was only read over adb.

## Why a binary patch (and not a kernel build)

* **`XBurst/Linux-X1000` cannot reproduce this kernel.**  The clone in
  `../../kernel-src/Linux-X1000` is *mainline Linux 5.0.0-rc1* (its `Makefile`
  says `VERSION = 5`, `PATCHLEVEL = 0`, `SUBLEVEL = 0`), a SoC bring-up
  skeleton: `drivers/video/fbdev/` has only `jz4740_fb.c` (the mainline driver,
  no SLCD, no board pdata, no panel init table), there is **no `ingenicfb`**
  (grep: 0 hits), **no `ak4493`** (0 hits), and `x1000.dtsi` carries only
  CPU/INTC/CGU/RTC/pinctrl/UART/MMC/DMA.  It would be a port of the display,
  touch, audio, PMIC, WiFi/BT, MMC and USB subsystems -- a different project.
  (The historical session `ses_ed33d4a15ffeeBziLlJrKTZldF` reached the same
  conclusion.)
* The stock kernel's `jzfb_blank` is self-contained and the PM path already
  contains the exact sequence that is needed, so an in-place patch is small and
  verifiable by disassembly.

## Provenance

All of the stock files below come from the vendor's `v3_analog` firmware, unpacked
in the older working copy (`firmware/tt/...` there); they are not part of this
repository.

* Stock kernel: `firmware/tt/kernel.bin` (the raw image, base virtual address
  `0x80010000`, so `file offset = virt - 0x80010000`), md5
  `aaa60f4fc2b4d592f92bc5a405e10695`.  It is byte-for-byte the gzip payload of
  `firmware/tt/iso/UIMAGE.BIN` (md5 `35889275f8bed2f967a18f6a8b66495c`), a
  u-boot uImage (`magic 0x27051956`, `load 0x80010000`, `ep 0x8051ce20`,
  `comp gzip`, name `Linux-3.10.14`).
* Stock firmware: `firmware/tt/fw.upt`, md5 `24bb64717ec6359bf779c57ae7c149d8`.
* Disassembler used for every address below:
  `sonix-player/rockbox-toolchain/bin/mipsel-rockbox-linux-gnu-objdump -D -b binary
  -m mips:isa32 -EL --start-address=<off> --stop-address=<off>`.
* Kernel source reference: `https://github.com/XBurst/Linux-X1000` (cloned in the
  older working copy's `kernel-src/`; assessed above, not used).

## What the stock code does (all offsets are file offsets in `kernel.bin`)

`jzfb_blank(int blank, struct fb_info *info)` at `0x1f3f94`
(virt `0x80203f94`).  `s0 = info->par` (the driver's `struct jzfb`); the
`fb_info` is `*(jzfb + 0x54)`, the register base is `*(jzfb + 0x60)`, the
`enabled/blank` flag is `jzfb->0`.

* **UNBLANK** (`0x1f3fb0`..`0x1f3ffc`): `LCDC_STATE=0`, clear `regs[264]`, set
  `LCDC_CTRL = (ctrl & ~0x10) | 0x8`, lock/unlock, clear a resume flag.  No
  pdata call, **no `SLCDC_CTRL.DMA_EN`** -> the panel is armed but shows nothing.
* **POWERDOWN** (`0x1f4000`..`0x1f4024`): `LCDC_CTRL &= ~0x8`,
  `SLCDC_CTRL &= ~0x1`.  No pdata call -> the rails stay up.

The power cycle lives only in the PM path:

* `lcd_power_off` (`0x8001a038`): disable AXP LDO7 (`lcd_3v3`) then LDO8
  (`lcd_1v8`), drive `gpio16`/`gpio48` low, pinmux the SLCD pins to function 4,
  gate the touch controller.  Idempotent (global flag at `0x80702338`).
* `lcd_power_on` (`0x8001a1b0`): pinmux to function 1, set+enable LDO7/LDO8,
  `gpio48=1`, `gpio16=1` (reset released last).  Idempotent.
* `jzfb_set_par` (`0x80205ae8`): sends the 343-entry panel init table and writes
  `SLCDC_CTRL` with `DMA_EN` (register 164 = `0xa4`).
* `jzfb_enable` (`0x80204794`): waits for LCDC idle, `LCDC_STATE=0`,
  `LCDC_CTRL |= 0x8`, sets `enabled=1`.
* `jzfb_resume` (`0x80206614`) runs `lcd_power_on` -> `jzfb_set_par` ->
  `jzfb_enable` (verified working on the device, including a 598 s sleep).

## The patch

`patch_kernel.py` rewrites `jzfb_blank` in place, reusing the bytes of the
function itself, so the decompressed image does **not** grow:

* Prologue `0x1f3fa8`: `bnez a0, 0x1f4000` becomes `bnez a0, 0x1f3fd4`.
* UNBLANK `0x1f3fb0` (9 words): `jal lcd_power_on; jal jzfb_set_par;
  jal jzfb_enable` -- each `jal`'s delay slot loads `a0 = *(jzfb+0x54)` (the
  `fb_info`) with `lw a0,84(s0)`; then `li v0,1; b 0x1f4028; sw v0,0(s0)`.
* POWERDOWN cave `0x1f3fd4` (13 words): clear `LCDC_CTRL.ENA` and
  `SLCDC_CTRL.DMA_EN`, then `jal lcd_power_off` with `sw zero,0(s0)` in the
  delay slot, then `b 0x1f4028; nop`.
* The old inline POWERDOWN block at `0x1f4008`..`0x1f4024` is left in place but
  is now unreachable.

```
$ python3 patch_kernel.py
    stock kernel : 7126564 bytes, md5 aaa60f4fc2b4d592f92bc5a405e10695
    patched kernel: 7126564 bytes, md5 dcf9a0ec779a80350374b6e522e90c83
    payload      : 3234226 bytes (stock 3236378)
    .../UIMAGE.BIN: 3234290 bytes, md5 3c0e15c26fb32636ef92ebac6a0a2ceb
```

The script refuses any input whose md5 is not the stock kernel.  It gzip's the
patched image exactly like the stock uImage (`mtime=0`, OS byte 3) and rebuilds
the uImage header with the same `load`/`ep`/name and fresh size/dcrc/hcrc.  The
patched uImage is **2152 bytes smaller** than the stock one.

Verify with:
```
objdump -D -b binary -m mips:isa32 -EL --start-address=0x1f3fa8 \
        --stop-address=0x1f4008 <patched kernel.bin>
```
`jal 0x1a1b0` / `jal 0x205ae8` / `jal 0x204794` / `jal 0x1a038` (shown
relative to base 0; add `0x80000000`) and `bnez a0,0x203fd4` are the patch.

## Carrying the patched kernel through the packer

The V3 Analog packer had no kernel-replacement mechanism (it carried the stock
`UIMAGE.BIN` untouched).  Added, mirroring the upstream Blaze/R1/R3PII rule
(`UPSTREAM-README.md` lines 316-323/397):

* `sonix-packer/kernel/UIMAGE.BIN` -- the patched kernel (also kept in this
  directory).  `$KERNEL_IMAGE` points elsewhere if wanted.
* `sonix-packer/v3_analog_packer.sh` -- when `kernel/UIMAGE.BIN` exists it is
  installed over the stock one; its size must not exceed the stock image, or the
  packer refuses.  `UPDATE.TXT`'s kernel md5 is recomputed.
* `sonix-packer/tools/fix_update_md5.py` -- extended to rewrite the `kernel={}`
  block from `UIMAGE.BIN` as well as the `rootfs={}` block from `SYSTEM.UBI`
  (matched by file name; the old one-argument form still works).  The verify
  step now checks both digests.

Reproduce end to end:
```
cd patch/v3-panel-blank
python3 patch_kernel.py            # writes ./UIMAGE.BIN
cp UIMAGE.BIN ../../sonix-packer/kernel/UIMAGE.BIN
cd ../../sonix-packer
STOCK=<the vendor's 1.3 .upt> ./v3_analog_packer.sh
```
(The default `STOCK` path `v3_analog_original.upt` is not in the repository; the stock
image is the vendor's 1.3 `.upt`, which has to be put there or named with `STOCK=` --
see `docs/V3-ANALOG.md`, "What a fresh clone needs".)

## Artifact

* `sonix-packer/out/v3_analog.upt` -- **58175488 bytes**, md5
  **`721a279c2031c432c2117fb4647a2120`**.
  * `UIMAGE.BIN` inside = md5 `3c0e15c26fb32636ef92ebac6a0a2ceb` (the patched
    kernel); `UPDATE.TXT` kernel md5 = the same.
  * `UBOOT.BIN` inside = md5 `1f54858d6b4e6f482a81a3378c0e5bf8` = the stock
    bootloader, untouched (do not write it; see the v3-firmware skill).
  * `SYSTEM.UBI` inside = md5 `2a0e77f8e6ea23ba61909234dd0e85c3`; `UPDATE.TXT`
    rootfs md5 = the same.
* The previous image is kept as
  `sonix-packer/out/v3_analog-rc1-151020262247.upt` (57923584 bytes) and at
  `/tmp/opencode/v3_analog.upt.prev` (md5 `7c4d79a6c8381799df33e23097461099`).

## Verification after flashing (read-only)

Flash with the image in the card root named `v3_analog.upt` via recovery (Power + Prev);
do not write the bootloader.  After the update, with the panel on:

```
# rails: LDO7 (lcd_3v3) and LDO8 (lcd_1v8)
for d in /sys/class/regulator/*/; do n=$(cat $d/name); case $n in *LDO7|*LDO8) echo "$n $(cat $d/state)";; esac; done
# reset pins: 16 = lcd rst, 48 = lcd rd  (dir/level; exporting them is EBUSY, that is expected)
cat /sys/class/graphics/fb0/blank
```

Then `echo 4 > /sys/class/graphics/fb0/blank` (POWERDOWN) and check:

* `regulator.12` (LDO7) and `regulator.13` (LDO8) `state` become `disabled`;
* `gpio16`/`gpio48` go low (watch `/proc/jz/gpio` or the pinmux, since the pins
  are `gpio_request`ed and cannot be exported);
* `SLCDC_CTRL` bit 0 (`DMA_EN`) is clear and the picture goes away.

Then `echo 0 > /sys/class/graphics/fb0/blank` (UNBLANK) and check:

* both rails `enabled` again, pins high;
* the panel's own init table is re-sent (`jzfb_set_par`), `SLCDC_CTRL.DMA_EN`
  set, and **the picture returns** (this is the whole point -- an unblank that
  only re-armed the controller used to leave the glass frozen);
* `/proc/irq/123/slcd_vsync` keeps ticking (panel scanning).

`cat /sys/class/graphics/fb0/blank` should read `0` after an unblank.

## Risks

* **Frozen glass** is the failure class this patch exists to remove, but it is
  also the one to watch: if `jzfb_set_par` fails (it can return `-EINVAL` if
  `info->var` is not acceptable) `DMA_EN` would not be restored and the glass
  would stay blank while the panel is powered.  If that shows up, the fallback
  is the already-proven PM cycle (`echo mem > /sys/power/state`, wake).
* Calling `jzfb_set_par`/`jzfb_enable` from the blank path takes the same
  driver mutexes the PM path takes; the PM path is verified on the device, but a
  blank issued *while* a suspend is in flight has not been exercised.
* `lcd_power_off`/`lcd_power_on` sleep (regulator ops); `fb_blank` is allowed to
  sleep, so this is within contract, but the blank path is now slower.
* The kernel image changed, so the running kernel is not the stock one; keep the
  `kernel.bin`/`UIMAGE.BIN` copies for a revert.

## What the player side needs (not changed here)

The port already has the machinery; it is switched off for this board:

* `src/system/device/board.h` (V3 board block, line ~141) sets
  `.blank_panel = false`, with a comment explaining exactly this defect (the
  blank leaves the SLCDC disarmed and an unblank does not undo it).
* `src/main.c` (~line 3941) turns that into
  `.blank_path = "/sys/class/graphics/fb0/blank"` only when `blank_panel` is
  true; `src/system/device/power.c` (~line 229) writes
  `FB_BLANK_POWERDOWN` (4) on screen-off and `FB_BLANK_UNBLANK` (0) on screen-on,
  and `display_wake_begin()` arms the panel afterwards.
* **To flip:** set `.blank_panel = true` for the V3 board.  With this kernel the
  blank is a real power cycle, so the screen-off path can power the panel down
  while the SoC stays awake (music playing, or before standby) -- which is the
  point of the request.  Because the driver now re-inits the panel on unblank,
  the extra `FBIOPUT_VSCREENINFO`/`FBIOPAN_DISPLAY` the arm path does is a second
  `set_par`; harmless, but the player could skip its own re-init once this kernel
  is in use.

## Revert

* Rebuild the image without `sonix-packer/kernel/UIMAGE.BIN` (delete or rename
  it) and re-run `v3_analog_packer.sh`; the stock `UIMAGE.BIN` is then carried
  through and `UPDATE.TXT`'s kernel md5 returns to
  `35889275f8bed2f967a18f6a8b66495c`.
* Or restore `sonix-packer/out/v3_analog-rc1-151020262247.upt`.
* The kernel patch itself is `patch_kernel.py`; reverting it is simply not
  running it.  The stock `firmware/tt/kernel.bin` and `firmware/tt/iso/UIMAGE.BIN`
  (in the older working copy) are never modified in place.

---

## Second iteration (2026-10-17): the hang, a capture, and a fix that is *not* confirmed

The first image (`out/v3_analog.upt` md5 `721a279c…`) was flashed and a
`echo 4 > /sys/class/graphics/fb0/blank` reset the device.  A deliberate
live capture (`/mnt/sd_0/kmsg-capture.txt`, a `dmesg` loop) was taken on that
kernel.  Two things came out of it.

### 1. The cause the brief suspected is real (code evidence)

`/proc/kallsyms` on the device resolves the calls the disassembly showed:

```
lcd_power_off  0x8001a038  ...  jal goodix_ts_close   (0x80363bfc)
lcd_power_on   0x8001a1b0  ...  jal goodix_ts_open    (0x80363b1c)
```

`goodix_ts_close` (file `0x353bfc`) takes **no argument** (it reads the global
`ts` at `0x8072f518`) and is **idempotent** — `if (ts->0xe0) return;`.  It calls
`gtp_enter_doze` (0x80362884), which writes the touch power-down command
`[0x80,0x46,0x08]` through `gtp_i2c_write`, retrying 5×6 times and calling
`gtp_reset_guitar` (with `__delay` timing) on every failure.  `lcd_power_off`
calls this **after** it has disabled LDO7/LDO8 — the rails that power the touch
— so the i2c transfer never completes and the CPU ends up in `__delay`; the
3 s watchdog then resets.  The next boot's dmesg confirms:

```
[    0.000000] CPU0 RESET ERROR PC:801E88C0
[    0.000000] [<801e88c0>] __delay+0x0/0x10
```

The PM path is safe for exactly the reason the brief gave: by then the goodix
driver's own suspend has already run, so the copy inside `lcd_power_off`
returns early.

### 2. The fix, and why it is *not* confirmed

`patch_kernel.py` now calls `goodix_ts_close` (0x80363bfc) at the **start** of
the POWERDOWN branch, while the rails are still up, before `lcd_power_off`; its
own copy then no-ops.  It still fits inside `jzfb_blank`'s bytes:

```
1f3fd4: jal 0x80363bfc        # goodix_ts_close (rails up)
1f3fd8: nop
1f3fdc: lw v0,96(s0)          # ... clear LCDC_CTRL.ENA / SLCDC_CTRL.DMA_EN
1f4000: jal 0x8001a038        # lcd_power_off (its goodix_ts_close now no-ops)
```

**But the capture shows the blank node was already crashing before the
watchdog**, and for *both* values:

```
[  496.608235] jz-fb jz-fb: the slcd  slcd_cfg_new is 00000200
[  496.608272] CPU 0 Unable to handle kernel paging request at virtual address 00000000, epc == 00000000, ra == 00000000
[  496.608279] Oops[#1]:
[  496.608292] CPU: 0 PID: 501 Comm: sh Tainted: G        W    3.10.14 #1
[  496.608405] epc   : 00000000   (null)
[  496.608412] ra    : 00000000   (null)
[  496.608438] Process sh (pid: 501, ...)
[  496.608531] Call Trace:
[  496.608553] [<8002249c>] stack_done+0x20/0x44
[  496.608565] Code: (Bad address in epc)
```

`echo 0` (UNBLANK) crashes right after `jzfb_set_par`'s `slcd_cfg_new` message;
`echo 4` (POWERDOWN) crashes in `lcd_power_off`.  Both are code this patch adds,
and both work fine in the PM path (`jzfb_resume`/`jzfb_suspend`).  The obvious
difference is context: the sysfs `blank` write runs under `console_lock`
(`store_blank`), the PM path does not.  I could **not** confirm that
`console_lock` is the cause — none of the pdata functions take `console_lock`
directly.

**Conclusion: the goodix ordering is a correct and necessary part of the fix,
but the blank path on this kernel is not safe to drive the full pdata power
sequence from, and I could not finish diagnosing that.  The new image is
therefore NOT verified.**
