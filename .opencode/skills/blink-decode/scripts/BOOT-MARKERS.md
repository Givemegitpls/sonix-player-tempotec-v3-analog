# Watching this kernel boot: the backlight-as-progress method, as it stands

The board has a console (`console=ttyS2,115200n8`) but it lives on a pad nobody can reach, the
panel's own text is unreadable (see §5), and a kernel that dies early leaves no adb. What works is
**blinking the backlight** from points of the boot and letting the owner count.

Everything here was learned the hard way; read §4 and §5 before writing any marker.

## 1. The signal

- Backlight power is **GPIO 89** = port C bit 25, named `BL PWR` in the vendor's dmesg.
- Registers (physical, from the SDK's `arch/mips/xburst/soc-x1000/common/gpio.c`): port base
  `0x10010200` for port C (`0x10010000` + port * 0x100), `PXINT` +0x10, `PXMSK` +0x20,
  `PXPAT1` +0x30, `PXPAT0` +0x40, `PXOENC` +0x68 (write the bit = output enabled), `PXPENS` +0x74.
- A pin's function is the four bits `(PXINT, PXMSK, PXPAT1, PXPAT0)`; the level is in `PXPAT0`.
- **The sequence was verified from userspace first**: `devmem` writes to exactly those registers
  on the running stock kernel made the backlight blink (`tools/blink_test.sh`).
- **The polarity is not obvious**: the pin is not in GPIO mode on a stock boot (its function bits
  are clear, the backlight is driven by PWM), so what `PXPAT0` reads tells you nothing about "on".
  Restore the four function registers when done and the PWM takes the pin back.
- The owner counts **dark periods** ("отключения экрана"), not flashes: one marker is 0.2 s of
  light and 1.2 s of dark, so a marker is one blackout either way. Counting the same message twice
  is cheap; the boot repeats anyway (`CONFIG_PANIC_TIMEOUT`).

## 2. What is marked now (2026-10-17)

Only two things, because everything the boot is known to pass costs nothing but counting:

| where | what it blinks |
|---|---|
| `drivers/mtd/nand/jz_sfcnand.c` (`jz_sfc_probe`, checkpoints A--E) | **1** blink each, in order: entered -> resources and clocks taken -> the chip's parameters read (**1** if from the flash's `nand_magic`, **2** if from the board's table) -> the flash chip detected -> the MTD partitions created |
| `v3analog/common/bootblink.c`, a panic notifier | **2** blinks header, then four values, each as (value + 1) blinks: how many **MTD devices** exist, whether the UBI volume **`rootfs`** opens (2/1), whether the MTD partition `rootfs` exists (2/1), whether the UBI volume **`data`** opens (2/1) |

The notifier is the important one: it runs *whenever* the kernel panics, which is exactly when the
state matters, and it does not depend on our own initcall being reached (an earlier version was a
`late_initcall_sync` and never ran -- the boot dies *inside* the last entry of that same group).

Call sites used earlier and removed once answered: the assembly stages in `head.S` (through the
trampoline into `start_kernel`), `start_kernel`/`setup_arch`/`plat_mem_setup`, `rest_init`, the
early-initcall loop, `sched_init_smp`, the initcall levels, and an "odometer" that blinked once per
ten initcalls. The odometer is in `init/main.c` (`V3_INITCALLS_PER_BLINK`) and the ordinal counter
`v3_initcall_ordinal` is still updated there -- the panic notifier used to blink it.

`tools/v3_report.py` decodes the two kinds of report (`count N` for the old odometer, `groups 3 3 7
7` for the panic's header-and-digits form) and prints the initcall name for an ordinal;
`tools/initcalls_by_addr.txt` is the build's initcall list in link order that it reads (regenerate
it with the `nm` one-liner above when the config changes).

Delay helpers, and why they are hand-written: `bl_delay()` in `v3analog/common/bootblink.c` is the
same three-instruction loop the assembly markers use (`addiu`/`bne`/`nop`), with `BL_PULSE` =
0x04000000 (~0.2 s) and `BL_GAP` = 0x18000000 (~1.2 s). A C loop with a plain counter is dead code
to the compiler and a `volatile` one has no predictable length (400 000 iterations came out
anywhere between 2 and 25 ms) -- do not go back to one.

## 2a. Build 6 (2026-10-19): the channels differ by the dark-run length

Everything the boot says now comes out as one of two blink lengths, so a **recording of the screen**
(analysed by a script, not by eye) tells which channel a dark run belongs to without counting
groups or guessing:

| channel | dark half | who uses it |
|---|---|---|
| checkpoint | ~0.2 s (`BL_PULSE`) | the build number, the SFC checkpoints A--E, the panic header (2 blinks), the read-failure step codes and nibbles |
| digit | ~0.6 s (`BL_DIGIT`, 0x0C000000) | the panic's error index and the four rootfs values |

Everything else is unchanged: light half 0.2 s, group gap 1.2 s, ~3 s between panic values
(`bl_delay(0x40000000)`), the panic notifier then reboots (`CONFIG_PANIC_TIMEOUT`) and repeats.

`bl_emit_value()` blinks via `bl_blink(value + 1, BL_PULSE, BL_DIGIT)`; `bootblink_raw(count)` is
the same primitive with both halves at `BL_PULSE`. Do not shorten the digit half: a phone camera's
exposure inertia merges two 0.2 s blinks into one 0.4 s dark run (that is what the first recording
of build 5 looked like), while 0.6 vs 0.2 stays separated even with the inertia.

Recording and decoding the timeline: record the screen with a phone camera (the C270 webcam's auto
exposure eats short flashes, and 720p YUYV runs at 7.5 fps -- force MJPEG if a webcam must be used),
then

```sh
scripts/blink_decode.py <video.mp4>     # dark/lit runs, timestamps, gaps
scripts/v3_report.py groups <g1> <g2> ...   # panic digits -> ordinal -> initcall name
```

The phone recording carries three luminance levels and they are all real signal:

| level | what it is |
|---|---|
| dark (0) | the marker's off half -- **the marker stream is the sequence of no-light frames; count these** |
| half (~85 on a black screen) | the idle backlight, PWM-driven |
| max (~140-150, a couple of frames) | the marker's on half: the GPIO drives the pin directly, no PWM, so a marker shines brighter than the idle backlight; the decay 143 -> 116 inside it is the phone's exposure closing |

This also means the phone's exposure inertia works for us, not only against us: a 0.4 s dark run =
one marker (its light and dark halves merged), while dark runs separated by ~0.9 s are neighbours
of one value (in the code consecutive blinks are exactly 2 x `BL_PULSE` apart). Long darks
(~25 s) are reboots + the slow boot, not markers.

The first reading of build 5 (phone video, 00:18 2026-10-19): the first ~21 s of the video are the
recovery writing the image (its progress screen produces dark runs too -- do not count them as
markers), then the reboot, then the slow boot in the dark (~25 s), then 12 diagnostic groups /
26 no-light runs between 44 and 126 s -- the panic diagnostics, with pairs merged by exposure
inertia (see the table above); ~70 s of dark at the end is the next cycle's reboot + slow boot.

## 3. What was actually observed (2026-10-17, in order)

| reading | what it meant |
|---|---|
| `1 2 3 4`, then a ~1 s flicker | the assembly stages, then `start_kernel`/`setup_arch`/`plat_mem_setup` with 1 ms delays |
| 12 blinks, then the logo | every initcall up to `console_init()` and into the rc level, with the old named scheme |
| 26 blinks | the early initcalls all ran; the death was in the next handful of statements |
| `3 3 7 7` (header + digits 2, 6, 6) | **ordinal 266** = the last entry of the initcall loop, i.e. *every* initcall ran and the panic is after them: mounting the rootfs or starting init |
| `2111` (header 2, then 0/0/0) | the panic diagnostic's first form: **no MTD at all**, no UBI volume, no `rootfs` partition |
| `1`, then `2 1 1 1` | the SFC driver was entered (checkpoint A) but never got past taking its clocks: the driver asked for a clock named `cgu_ssi` that this SDK calls `cgu_sfc` (see `SFC-NAND.md`) |
| `1 1 1 1 1`, then `2 7 1 2` | with the clock alias added the whole SFC path works: **6 MTD devices**, the `rootfs` partition exists, but the UBI volume still does not open |

## 4. The traps that cost the most time

1. **Delay loops get optimised away.** A plain `for (spin = 0; spin < N; spin++);` with a
   non-volatile counter is dead code: gcc removes it and the "blink" becomes microseconds wide.
   The counter must be `volatile` -- and the *count* must be big enough to see. **Do not guess the
   C loop's cost**: use `bl_delay()`, whose timing is already known, and check it in the
   disassembly (the loop body must be exactly `addiu`/`bne`/`nop`).
2. **Only a physical check counts.** "No blinks" has two explanations (the kernel died, or the
   signal does not work), and they look identical. Validate the signal from userspace with
   `devmem` first.
3. **A marker can change the thing it measures.** Two ways, both seen here:
   - the first version left the backlight off (wrong polarity) so the screen went dark and the
     bootloader's logo disappeared -- which read as "the kernel killed the display";
   - the marker builds slowed the boot by ~35 s, and the boot *got further* with them. The same
     build without markers did not reach the rootfs stage at all. Treat "it boots further when I
     add markers" as a finding about timing, not as a licence to keep them: the current builds
     carry a **silent** `msleep(V3_INITCALL_DELAY_MS)` (120 ms before every initcall) for the same
     reason, and it must go once the rootfs mounts.
4. **`bootblink()` before gpiolib does nothing at all.** `jz_gpiolib_init()` is called *inside*
   `setup_gpio_pins()`, so any marker before that one must use `bootblink_raw()`; `bootblink()`
   goes through `gpio_request`/`gpio_set_value`, which silently fail there. Markers in
   `board_base_init` (an earlier arch_initcall than `setup_gpio_pins`) were dead until this was
   noticed.
5. **`late_initcall_sync` markers may never run.** The panic happens inside the last entry of that
   group (`initialize_hashrnd`), so an initcall of ours at the same level can be after it. Put any
   "state at the end" diagnostic in a **panic notifier**.
6. **Do not remove markers with a regex over a whole file.** A pattern like
   `\{\n\s*/\*.*?\*/\n\s*extern ...` is not anchored to a line start, starts at any lone `{`, and
   with `re.S` swallows the code between two markers: it deleted ~6200 lines of
   `kernel/sched/core.c` and ~790 of `kernel/softirq.c` in one go. Remove them by hand, or restore
   the file with `git checkout HEAD -- <file>` when our markers were its only change.
7. **Instrument the file that is actually built.** Two SFC drivers exist and the config picks one;
   markers written into the unused file compile away silently and the boot looks as if nothing
   happened. Check that the symbol you edited is in the *built* `vmlinux`.

## 5. The panel draws, and that is the future "log"

The boot's screen is not blank: it shows a lattice of coloured dots, and sometimes a legible
penguin (the Linux logo). Measurements from a photo: the lattice covers about 320x320 of the
320x480 panel, dots ~4 px apart, and glyph-like clusters are visible inside it. That is the
framebuffer being transferred to the glass with a **sample step of about four pixels** -- the
console's text is there, its pixels are just spaced out.

Two consequences:

- **Writing into the framebuffer is not enough to see it**: a frame reaches the glass only through
  the driver's `fb_pan_display` (it swaps the descriptor and restarts the DMA). Writing the memory
  alone changes nothing visible.
- A panic-notifier experiment that **cleared** the framebuffer to black did change the glass (the
  dots disappeared) -- so the pan path does work from that context -- but the glyphs drawn
  afterwards at a 4-px step did **not** appear. Why is still open. If it can be made to work, the
  panic message can be read off the glass and the blinks become unnecessary.

Not available: a USB serial console. `console=ttyGS0` is on our command line, but this tree's
`drivers/usb/gadget/serial.c`/`u_serial.c` contain **no `register_console` at all** and
`CONFIG_USB_G_SERIAL` is off, so there is nothing for the name to attach to. The android gadget
(`CONFIG_USB_G_ANDROID`) needs userspace to enable it, which is what we are trying to reach.
