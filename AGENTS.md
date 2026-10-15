# Notes for agents working on this repo

Sonix Player ported to the TempoTec V3 Analog (Ingenic X1000, 320x480 SLCD panel, AK4493 DAC,
glibc 2.16). The player source is `sonix-player/`, the firmware packer `sonix-packer/`, the
target-side tools `tools/`.

## Read the knowledge base before touching anything

The chronological log is `../.opencode/NOTES.md` (long, and the place where every measurement
ends up). Three consolidated documents sit next to this file:

- `docs/V3-ANALOG.md` — what this port is, what differs from the Blaze port, what does not work.
- `docs/BLUETOOTH.md` — the BlueZ 5 stack, the codecs, the LDAC/MTU defects and their patches,
  the A2DP encoder priority story, the tools and the verification recipes.
- `docs/DISPLAY.md` — the SLCD panel: the pan model, the panel's own 13.232 ms frame, the pacing
  design, the freeze bug, and what "display off" can and cannot mean on this board.
- `docs/KERNEL.md` — the stock kernel's addresses and functions, the board platform data, and how
  a replacement kernel would (and would not) reach the image.

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
- `git commit` is denied to agents by the global opencode config — stage, write the message, and
  hand the command to the owner.
- The packer's image is the deliverable, not a file on the card: `sonix-packer/out/v3_analog.upt`.
  A replacement kernel may not be larger than the stock one, and `UPDATE.TXT` carries digests for
  both the rootfs and the kernel.
- The panel's frame rate is the panel's own (75.57 Hz). Nothing on the SoC side changes it.
- Never blank this panel: `blank_panel = false` is deliberate (see `docs/DISPLAY.md`).
- Keep the big build inputs out of git: `bt-src/sysroot/`, `bt-src/debs/`, `kernel-src/` clones.
  Their patches, pc files and READMEs belong in the repo; the tarballs and sysroots do not.
