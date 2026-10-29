# The kernel: what it is, what is known about it, how a replacement would reach the image

Consolidated from the chronological log (`.opencode/NOTES.md`) and the session
archive (`tools/history.sh`; session ids at the end).
The kernel is HiBy's; the port does not ship one.

## The image and the addresses

- Stock kernel: the `UIMAGE.BIN` inside the vendor's `v3_analog.upt`, md5
  `35889275f8bed2f967a18f6a8b66495c`, 3.10-era, carried into every image **untouched**.
  The `.upt` is a gitignored file of the tree (`sonix-packer/v3_analog_original.upt`,
  the owner's own vendor image), its `UIMAGE.BIN` is unpacked at
  `sonix-packer/build/iso/UIMAGE.BIN` (also gitignored), and the decompressed
  kernel (`kernel.bin`, 7126564 bytes) is not part of this project: take the
  `UIMAGE.BIN` out of your own vendor `.upt` and `gzip -d` its payload to get it.
- The panel-blank patch's output is `patch/v3-panel-blank/UIMAGE.BIN`: 3234293 bytes, uImage
  md5 `f6384f647c8b2013a23a18a035933ad6`, kernel md5 `c99734a4a8f30b37aa932f6940fc0767` --
  byte-for-byte what `patch/v3-panel-blank/patch_kernel.py` builds from the stock kernel. It is
  **not in git** (`.gitignore`): it is HiBy's binary with our patch written into it, and their
  kernel source was never published, so it is rebuilt locally from the vendor `.upt` and kept
  out of git. The README records the md5s an earlier revision of the script produced
  (`dcf9a0ec…` kernel, `3c0e15c2…` uImage, 3234290 bytes).
- Base: virt `0x80010000`, and **file offset = virt − 0x80010000**. Everything below is quoted
  that way; disassemble with
  `sonix-player/rockbox-toolchain/bin/mipsel-rockbox-linux-gnu-objdump -D -b binary -m mips:isa32 -EL --start-address=<off> --stop-address=<off>`.

## The board's platform data (the map that matters)

At virt `0x80652828` (file `0x642828`), size `0x94`:

```
+0x00 num_modes = 1            +0x04 modes = &videomode (320x480, pixclock 13333, name "ssd2805")
+0x0c lcd_type = 0x8000000d (SLCD)   +0x10 bpp = 24   +0x30 bus_width = 8   +0x34 = 3
+0x38 length_data_table = 343  +0x3c data_table = 0x80652924
+0x40 power_io_init   +0x50 set_window   +0x54 set_dc
+0x68 lcd_power_on    +0x6c lcd_power_off  +0x80 init_vsync  +0x84 uninit_vsync  +0x90 lcd_rotate
+0x94 write_gram_cmd[0] = 0x2c2c2c2c   <- the same value seen as desc_cmd[0] on the device
```

The panel's init table is **343 entries of `{type, value}`** (type 0 = command, 1 = data byte,
2 = `udelay`) at file `0x642924`, and it is a real sequence (`B9 00 00 / BA 21 01 / B9 01 00 /
BB 90 00 / D6 05 01 / B7 50 02 / B8 00 00 / BE 40 01 / BC 03 00 / BD 00 00`, then `F0 AA 55 52 00`,
`11`, `FD 05`, paged writes `BF <addr> …`, gammas `E0`-`E5`, `2A 00 00 01 3F` / `2B 00 00 01 DF` /
`2C`). U-Boot carries the same table (file `0x53718` in a dump of the device's `mtd0`, taken
with `adb shell 'dd if=/dev/mtd0 of=/data/mtd0.bin bs=1M'` + `adb pull`; the dump is not part
of this repository) with different `udelay` values.

## The functions, by address

| symbol | virt | what it does |
|---|---|---|
| `jzfb_set_par` | `0x80205ae8` | sends the 343-entry table **on every `FBIOPUT_VSCREENINFO`**, and sets `SLCDC_CTRL.DMA_EN` (file `0x1f6580`/`0x1f6598`) |
| `jzfb_blank` | `0x80203f94` | UNBLANK: `LCDC_STATE=0`, `LCDC_CTRL \|= ENA`. Any other level: `LCDC_CTRL &= ~ENA`, `SLCDC_CTRL &= ~DMA_EN`. **No pdata calls, so no panel power** |
| `jzfb_pan_display` | `0x802058f4` | swaps the descriptor, `SLCDC_CTRL \|= DMA_START\|DMA_MODE` |
| `jzfb_enable` / `jzfb_disable` | `0x80204794` / `0x80203f20` | controller on/off |
| `jzfb_suspend` | `0x8020482c` | waits for `DMA_START`, disables, gates clocks, tail-calls `lcd_power_off` |
| `jzfb_resume` | `0x80206614` | `lcd_power_on` + `jzfb_set_par` + `jzfb_enable` + `init_vsync` |
| `jzfb_open` | `0x8020708c` | calls `jzfb_resume` while the flag at `ingenicfb+420` is set (first open after probe/suspend) |
| `lcd_power_on` | `0x8001a1b0` | SLCD pins to function 1, `set_voltage`+`enable` LDO7 (3.0 V) and LDO8 (1.8 V), then `gpio_direction_output(48,1)`, `(16,1)` — reset last |
| `lcd_power_off` | `0x8001a038` | `disable` LDO7 then LDO8, `gpio_direction_output(16,0)`, `(48,0)`, SLCD pins to function 4, tail call that gates the touch controller with the panel |
| `power_io_init` | `0x80019e20` | `gpio_request(16,"lcd rst")`, `gpio_request(48,"lcd rd")` — why export gives `EBUSY` |
| `slcd_send_mcu_command` / `_data` | `0x80204610` / `0x80204590` | the write-only command path the table goes through |

Names were recovered from the image's kallsyms (not compressed): `kallsyms_addresses` at file
`0x5739f0`, `num_syms` `0x53c3` at `0x588900`, `kallsyms_names` from `0x58890c`,
`kallsyms_token_table` from `0x5c3080`, `kallsyms_token_index` (256×u16) at `0x5c33c0`; the pairs
are `addrs[k] ↔ names[k+4]`.

## Two consequences worth remembering

- **The freeze class lives in `jzfb_blank`.** It clears `DMA_EN` and never restores it; `unblank`
  brings back only `ENA`, after which a pan reports success without a transfer. The port's fix is
  never to use that path on this panel (see `docs/DISPLAY.md` and `docs/PANEL-POWER.md`).
- **The stock panel power cycle is `SET_SLEEP`** (`jzfb_suspend`/`jzfb_resume`, the vendor ioctl
  `0x80044620`) on every screen-off, with suspend-to-RAM as a second, later event. From userspace
  the reachable routes are: that ioctl on `/dev/fb0`, `echo mem > /sys/power/state` (the player's
  standby), closing and reopening `/dev/fb0` (which runs `jzfb_resume`), and the physical recovery
  combo (Power + Prev). `docs/PANEL-POWER.md` has the evidence.

## How a kernel would reach the image

- Upstream (Blaze/R1/R3PII) the packer accepts **`kernel/xImage-<model>`**: if present it replaces
  the stock kernel, it **may not be larger than the stock one**, and its size and md5 go into the
  update metadata (`ota_update.in` upstream; `UPDATE.TXT` here, where the kernel md5 is currently
  copied from the original). Without an xImage the stock kernel is carried across untouched.
- Those xImages are built from **https://github.com/Jepl4r/hiby-custom-kernel** — a 4.4.94 kernel
  for that family; modules in `assets/<model>/module_driver/` are built against it.
- **The V3 Analog packer has the mechanism, under another name.** A kernel drops in at
  `sonix-packer/kernel/UIMAGE.BIN` (or wherever `KERNEL_IMAGE=` points): `v3_analog_packer.sh`
  refuses one that is not a uImage and one larger than the stock kernel, copies it in verbatim and
  recomputes the kernel digest in
  `UPDATE.TXT` (`sonix-packer/tools/fix_update_md5.py` rewrites both blocks). Without it the
  stock kernel is carried across untouched, which is the normal case -- see
  [BUILD.md](BUILD.md). The only kernel work this repository carries is the binary patch in
  `patch/v3-panel-blank/`, which explains its own build and flash.
- The V3 Analog's stock kernel is **3.10-era**, so the 4.4.94 tree does not fit it. The X1000
  source the owner keeps — **https://github.com/XBurst/Linux-X1000** — was assessed on 2026-10-12
  and is **not usable for this device**: it is mainline Linux 5.0.0-rc1 with `MACH_X1000`, a
  `cu10` board and an `x1000.dtsi` that covers only CPU/INTC/CGU/RTC/pinctrl/UART/MMC/DMA. There
  is **no display driver at all**, no AK4493 codec, and the defconfig has FB, DRM, INPUT_EVDEV,
  I2C, SOUND, MTD, USB and VFAT switched off — a SoC bring-up skeleton, not this player's kernel.
  Anything kernel-side for the V3 therefore means patching the stock image, not rebuilding from
  that tree.
- **No replacement kernel for this device is usable today.** The mechanism above exists for a
  kernel someone built or patched and verified themselves; the port ships the stock 3.10.14 image,
  and the one kernel experiment this repository records (`patch/v3-panel-blank/`) failed — the
  blank node Oopses under `console_lock`, and the vendor `SET_SLEEP` ioctl already does the power
  cycle it was written for. Treat a replacement kernel as unsupported: wrong bytes there brick the
  device and the only way back is the Ingenic Cloner.

## Archive

- `ses_ec6820354ffeMOuM0X72my4TUI` — "Декомпиляция slcd_cfg_new в kernel.bin плеера": the
  disassembly behind the table, the pdata map and the blank/DMA_EN finding.
- `ses_ed33d4a15ffeeBziLlJrKTZldF` — "Оптимизация прошивки и UI плеера" (2026-10-12): the X1000
  tree assessment quoted above, and the finding that the device's **rootfs is mounted rw and can
  be edited live over adb** (no repack needed for a rootfs-side experiment).
