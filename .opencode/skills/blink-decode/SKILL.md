---
name: blink-decode
description: >
  Use when the TempoTec V3 (or similar headless/embedded device) reports
  boot progress or a panic through backlight blinks — "посчитай мигания",
  "расшифруй блинки", "на экране мигает/гаснет", "после N миганий зависло",
  a video of the screen was recorded for blink analysis, or a new kernel
  build with bootblink markers was flashed and measured. Covers: getting a
  usable recording (phone camera; the C270 webcam eats short flashes),
  decoding the dark/light run timeline from any video, and mapping the
  pattern to boot stages, the initcall odometer, or the panic ordinal via
  BOOT-MARKERS.md and v3_report.py.
---

# Blink decoding for V3 boot diagnostics

The decoders live beside this file, in `scripts/`: `BOOT-MARKERS.md` is the master document,
`v3_report.py` the report tool, `blink_decode.py` the one that turns a recording into a timeline,
`initcalls_by_addr.txt` the numbered initcall list. Nothing else is needed to decode a recording.

The kernel side — `bootblink.c`, `v3_bootmark.h` and the board patch — is a modification of the
vendor kernel and is not shipped anywhere: the blinker has to be built into a kernel to produce
marks at all. Without it the message below is still readable by eye: the number of blinks is the
number of screen-offs.

The blinker blinks the backlight at
boot checkpoints because there is no other console. The number of blinks =
the number of the last stage that ran. **The owner counts screen-offs (dark
periods), not flashes** — same number: one marker = 0.2 s light + dark gap.

## Step 1 — get a recording

- Phone camera pointed at the screen is the reliable source. A webcam lying
  right on the panel also works (frame = screen).
- **Logitech C270 webcam caveat:** its auto exposure integrates 15-130 ms and
  AGC renormalizes — short flashes get eaten. Also 720p defaults to YUYV =
  only 7.5 fps (bad timestamps); force `-input_format mjpeg` for 30 fps.
- If the webcam must be used, lock exposure manually (auto_exposure=1 +
  exposure_time_absolute) and verify the flag became active, else use a phone.
- Do not trust "no blinks seen" from any source before checking the signal
  path: `scripts/blink_test.sh` blinks from userspace and must be visible.

## Step 2 — decode the timeline

```sh
scripts/blink_decode.py <video.mp4>      # needs ffmpeg on PATH and numpy
```

Prints per-frame stats, the auto threshold, every dark run (backlight off)
and lit run with timestamps and the gap before each dark run. **The message is
the sequence of no-light frames**: the owner counts dark periods (screen-offs),
not flashes; one dark run = one marker pulse.

The phone recording carries three real luminance levels: dark (0), idle
half-brightness (PWM, ~85 on a black screen), and max (~140-150, a couple of
frames = the marker's GPIO-driven on-half, no PWM). Use max-flashes to verify
marker order; a ~0.4 s dark run is one marker (its light and dark halves merged
by exposure inertia), dark runs ~0.9 s apart are neighbours of one value. Long
darks (~25 s+) are reboots + the slow boot, not markers. The first ~21 s of a
recording right after a flash are the recovery's own screen -- its dark runs are
NOT markers.

Options: `--thr N` (auto threshold is a bimodal-histogram midpoint; override
if one outlier frame breaks it), `--fps F` (from ffprobe by default; VFR
phone video ≈ 27.5), `--frame N` (single-frame debug).

## Step 3 — interpret against the CURRENT scheme

Since build 6 the two channels differ by the **dark-run length** (checkpoints ~0.2 s, panic
digits ~0.6 s) — classify dark runs by length first, then read the scheme (see `BOOT-MARKERS.md`
§2a). Schemes changed many times before that (stages 1..4, then triplets, then one-blink-per-
stage with 12/17/26/31 stages, then the initcall odometer, then
diagnostics-in-panic-notifier). **Never decode from memory** — read the
scheme of the flashed build first:

1. `scripts/BOOT-MARKERS.md` — the master document.
2. The md5 of the kernel on the card (the card keeps each build under
   `v3_analog_<what>-<md5>.upt`, the name often records the previous
   measurement) — it says which build produced the marks.
3. `scripts/BOOT-MARKERS.md` again for the raw list of stages of that build.

Known protocols (verify against the build in question):

- **Stage counter (early builds):** N blinks = stage N passed, hang inside
  the next stage. Stage names live in the `bootblink.c` header.
- **Initcall odometer:** 1 blink per 10 initcalls; `scripts/v3_report.py count N`.
  Reference marks in this build: level 3 end = 3, level 4 end = 7, level 5
  end = 8, level 6 end = 24 blinks; fbcon dots appear ~21.
- **Panic sequence:** 3-blink header, then digit groups, group of N blinks =
  digit N-1; digits are read consecutively as an ordinal:
  `v3_report.py groups 3 3 7 7` → ordinal 266. Ordinal → initcall name via
  `scripts/initcalls_by_addr.txt` (link order = run order). If no panic (pure
  hang), the notifier stays silent — the odometer answers instead.
- **SFC control points A-E** (probe, resources, chip params, chip ID,
  partitions): blinks 1..5 in order.
- **Rootfs/UBI diagnostics (panic notifier):** header 2, then values each as
  value+1 blinks with ~3 s pauses: MTD count (6 → 7), UBI rootfs volume
  (2 = opens / 1 = no), `rootfs` MTD name (2/1), and later `data` volume (2/1).

## Step 4 — name the culprit

- Panic ordinal: `v3_report.py groups ...` prints the initcall name;
  `scripts/initcalls_by_addr.txt` has the full numbered list.
- Odometer: the culprit sits inside the next decade — blink each entry
  individually with a follow-up build.
- A hang with no panic between stages A and B: diff what runs between them
  from the disassembly (`nm -n vmlinux` on a kernel you built yourself) plus
  `scripts/initcalls_by_addr.txt`.

## Cautions that already cost time

- **Marker timing changes what is measured:** markers add ~1.4 s per stage;
  the slow boot reaches further (timing was the fix, not a bug). `msleep(120)`
  before each initcall keeps that effect without visual noise.
- Markers before `setup_gpio_pins` must be raw (`bootblink_raw`); gpiolib
  isn't up yet and `bootblink()` silently does nothing.
- A compiler may drop an unmarked delay loop; verify marker calls in the
  disassembly (`jal bootblink_raw` with `li a0,N` in the delay slot).
- The blinking blue LED (~2 s period) is the PMIC charge indicator, not our
  signal — it blinks on a dead device too.
- `v3_bootmark.h` writes stage markers to DRAM at 0x03F00000; readable via
  `devmem` if the device self-resets (DRAM survives reset, not power loss); the
  panic flight recorder needs the kernel-side helper that writes it.
- The panel may also show fbcon text (blocky, ~4 px sampling) or the panic
  notifier's 8x16 text — photograph the screen before rebooting; a picture
  of the screen often beats counting.
- The device reboots on panic (`CONFIG_PANIC_TIMEOUT`) and reboots when the
  stock player exits — a repeating blink cycle usually means panic-reboot.
