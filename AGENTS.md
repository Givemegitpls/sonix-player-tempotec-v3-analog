# Notes for agents working on this repo

Sonix Player ported to the TempoTec V3 Analog (Ingenic X1000, 320x480 SLCD panel, AK4493 DAC,
glibc 2.16). The player source is `sonix-player/`, the firmware packer `sonix-packer/`, the
target-side tools `tools/`.

## Read the knowledge base before touching anything

The chronological log is `.opencode/NOTES.md` in this repository (long, and the
place where every measurement ends up). The consolidated documents sit next to
this file:

- `docs/V3-ANALOG.md` — what this port is, what differs from the Blaze port it came
  from, what does not work.
- `docs/BLUETOOTH.md` — the BlueZ 5 stack, the codecs, the LDAC/MTU defects and their patches,
  the A2DP encoder priority story, the tools and the verification recipes.
- `docs/DISPLAY.md` — the SLCD panel: the pan model, the panel's own 13.232 ms frame, the pacing
  design, the freeze bug, and what "display off" can and cannot mean on this board.
- `docs/PANEL-POWER.md` — the stock `SET_SLEEP` panel power path, the evidence for it, and what
  the port now does; it corrects the power sections of `DISPLAY.md` and `V3-ANALOG.md`.
- `docs/POWER.md` — what costs battery, the state the panel is in for each of them, the audit of
  2026-10-24 and what it changed, and `tools/powsample`, the tool the numbers came from.
- `docs/KERNEL.md` — the stock kernel's addresses and functions, the board platform data, and how
  a replacement kernel would (and would not) reach the image.
- `docs/BUILD.md` — the end-to-end build, from a fresh clone to `sonix-packer/out/v3_analog.upt`.
- `docs/UPDATING.md` — taking a newer upstream Sonix release: the merge, the interface scaling,
  the icons and the language files, and what is not taken from it.

## The session archive is part of the record

`tools/history.sh` reads the opencode session database — the transcripts of how the harder parts
were worked out (the two-day LDAC debugging, the kernel disassembly, the panel freeze). Use it
before re-deriving something:

```
tools/history.sh sessions LDAC          # sessions mentioning a keyword
tools/history.sh grep "MTU"             # matching excerpts across all sessions
tools/history.sh text  <session-id>     # what was written in one session
tools/history.sh tools <session-id>     # tool calls with their outputs
tools/history.sh files <session-id>     # files a session touched
tools/history.sh snaps                  # file-state snapshots (unreferenced git objects)
```

## Rules that keep biting

- **adb is USB only.** Plain `adb`; never `adb connect`, never an IP or port. The device shows up
  as `0123456789ABCDEF`; if `adb devices` is empty, stop and say so.
- **Builds run in docker, not on the host.** `sonix-player/docker/build.sh` builds the player in
  `sonix-player/docker`'s image, `sonix-packer/v3_analog_packer.sh` packs in `sonix-packer/docker`'s.
  Do not apt-install build dependencies into the system to get a build going: the lists live in
  those two Dockerfiles, and a missing package is a Dockerfile edit. `docs/BUILD.md`,
  "Building on the host", is the escape hatch, not the default.
- `git commit` is denied to agents by the global opencode config — stage, write the message, and
  hand the command to the owner.
- The packer's image is the deliverable, not a file on the card: `sonix-packer/out/v3_analog.upt`.
  A replacement kernel may not be larger than the stock one, and `UPDATE.TXT` carries digests for
  both the rootfs and the kernel.
- The panel's frame rate is the panel's own (75.57 Hz). Nothing on the SoC side changes it.
- The interface is written at 480x720 and compiled at 320x480: every pixel size or position
  goes through `SC()` (`src/gui/shell/scale.h`, pulled in by `gui.h`). Upstream UI code arrives
  without it -- `sonix-player/tools/scale_gui.py` finds the bare literals in layout calls, and the `#define`d
  sizes at the top of each page have to be wrapped by hand. Nulls, counts, milliseconds and
  percentages are not sizes.
- The player's source follows upstream `Jepl4r/sonix-player`: an update is `git merge <tag>`,
  not a hand-applied diff. `docs/UPDATING.md` is the recipe, and the invariants it lists
  (`SET_SLEEP` instead of `FB_BLANK_POWERDOWN`, one framebuffer page and the pan, card 1,
  glibc 2.16, `/dev/mtd5` is `/data`, BlueZ 5) must survive every merge.
- Never write `FB_BLANK_POWERDOWN` on this panel: `blank_panel = false` is deliberate, the only
  `FBIOBLANK` the port issues is the `UNBLANK` in `fb_arm()`, and the stock screen-off is the
  vendor `SET_SLEEP` ioctl instead (`panel_sleep_ioctl`, `power.c`). See `docs/DISPLAY.md` and
  `docs/PANEL-POWER.md`.
- Keep the big build inputs out of git: `bt-src/sysroot/`, `bt-src/debs/`, `kernel-src/` clones.
  Their patches, pc files and READMEs belong in the repo; the tarballs and sysroots do not.
