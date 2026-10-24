# Project hints

Read this before building anything. It exists because one session spent three hours
rebuilding a toolchain that was already on disk.

## Where the build artifacts actually live

This repository is a **worktree** of `~/Projects/own/tempotec-v3/src/sonix-blaze`
(branch `own-kernel`). Gitignored build outputs stay in the parent, so a worktree
looks empty while the parent has:

- `~/Projects/own/tempotec-v3/toolchain/rockbox-toolchain` — the MIPS toolchain
  (binutils 2.38, gcc 9.5.0, glibc 2.27 `--enable-oldest-abi=2.16`), 350 MB;
- `~/Projects/own/tempotec-v3/src/sonix-blaze/sonix-player/freetype-target/` and
  `audio-target/` — the static libraries the player links;
- `.../sonix-player/build_target/`, `sonix_player`, `sonix_launch`;
- `.../sonix-packer/out/*.upt` — previously built images.

`make ROCKBOX_TOOLCHAIN_DIR=<dir> target` reuses a toolchain that already exists.
Check there **first**; a full `make target` from scratch is 20-60 minutes and is
only needed on a machine that has none.

## An incremental build proves nothing after a merge

`sonix-player/docker/build.sh` is incremental, and `make` compares mtimes against
`build_target/`: when the objects are newer than every source it prints
`Scanning source files...`, links nothing and exits 0. To check that a merge
actually compiles, force it -- `./sonix-player/docker/build.sh clean` wipes only
`build_*` and the binaries, the toolchain and the library prefixes stay -- and
then run the build again. The link ends with `check-abi: no symbol beyond glibc
2.16` and the stripped `sonix_player` (7.4 MB from a 40 MB debug binary).

This worktree carries its own `sonix-player/rockbox-toolchain` (350 MB),
`freetype-target/`, `audio-target/`, `build_target/` and
`sonix-packer/v3_analog_original.upt`, so it builds and packs without borrowing
anything from `~/Projects/own/tempotec-v3`.

## The toolchain is a build artifact, not a package

Building it means compiling binutils, gcc and glibc from source. Its steps 7-11
(`libffi`, `zlib`, `glib`, `expat`, `dbus`) build Bluetooth-userland libraries that
**this port does not link**: the player needs `libc` and `libasound` from the
sysroot, and the Bluetooth userland ships prebuilt in `sonix-packer/assets`. The
shipped toolchain has alsa and nothing else in its sysroot — that is not an
accident.

`sonix-player/Makefile` therefore passes `--without-bluetooth-libs` to
`rockboxdev.sh`: the script skips steps 7-11, finishes, cleans its workspace and
exits 0. `make check-toolchain` is still what catches an interrupted run (gcc
present, sysroot without `libc`/`libasound`).

## Host packages: none

`sonix-player/docker/build.sh` builds the player in `sonix-player/docker`'s image
(tree mounted at its own absolute path, container runs as the calling user).
`sonix-packer/v3_analog_packer.sh` builds the player itself when
`sonix-packer/sonix_player` is missing, then packs in `sonix-packer/docker`'s image.
Do not apt-install build dependencies on the host: add them to the Dockerfile.

## Rakes this project has already stepped on

- `sonix-packer/sonix_player` and `sonix_launch` used to go **stale**: the packer
  built them only when missing. It now runs `docker/build.sh` on every run
  (`make` is a no-op in a second when nothing changed); `SKIP_PLAYER_BUILD=1`
  opts out deliberately.
- `sonix-player/tmp/` is rockboxdev.sh's workspace (tarballs + extracted sources,
  2+ GB). Gitignored; a failed run leaves it behind, a successful one keeps the
  tarballs and drops the extracted sources.
- `STOCK=` must not point inside `sonix-packer/build/`: the script clears that
  directory before unpacking. It now refuses such a path.
- `rockboxdev.sh` keeps its sources extracted between runs, so patches must be
  applied idempotently; they now are (forward dry run, then reverse).
- An interrupted toolchain build leaves `bin/…-gcc` without `libc`, and `make`
  treats that file target as done. Both `docker/build.sh` and `make check-toolchain`
  check the sysroot instead of the binary.
- An external toolchain reaches the build through two things, and it needs both:
  `docker run`'s `-v`/`-e` for it, and `?=` rather than `=` on
  `ROCKBOX_TOOLCHAIN_DIR` in the Makefile. A plain `=` there ignores the
  environment and builds a second toolchain into the tree, which looks like the
  passthrough not working at all.
- The packer's `warn()` writes to stderr: `$(os_version)` is captured, and a
  warning on stdout ends up inside the version string written into
  `system-info.json` (the `sed` that does it then dies on the newline).

## Simulator (host build)

`make host` in `sonix-player/` builds `sonix_player_host` (SDL2, offscreen OK). Run:

```
SDL_VIDEODRIVER=offscreen SONIX_SD_ROOT=/tmp/opencode/sd ./sonix_player_host > /tmp/opencode/host.log 2>&1 &
```

- Input is injected by writing to `/tmp/sonix/tap`: `x y` is a tap, `drag x1 y1 x2 y2` a drag.
- `touch /tmp/sonix/shot` asks for a screenshot; the PNG lands in `$SONIX_SD_ROOT/Screenshots/`.
- The app consumes the tap/shot files, so they do not stay in `/tmp/sonix/`.
- The simulator needs the language files or every label shows its key:
  `SONIX_LANG_DIR=sonix-packer/assets/BLAZE/usr/resource/sonix/language`, and a
  fresh `SONIX_HOST_STATE_DIR` goes through the welcome flow again.
- `sonix-player/docker/build.sh host` does NOT build the simulator: the image
  has no ALSA/SDL host headers. `host` is for a machine with those installed --
  run `make host` in `sonix-player/` directly.

## Updating to a newer upstream Sonix release

The tree carries upstream's history, so an update is `git merge <tag>` (upstream
remote, tag fetched), not a diff to apply by hand. The base of this image is
1.3.0 (`a488e25`); `docs/V3-ANALOG.md`, "Upstream base: 1.3.0", has the recipe.
The part that is easy to underestimate:

- Upstream writes the interface at 480x720; this board compiles it at 320x480
  through `SC()` (src/gui/shell/scale.h, pulled in by gui.h). New upstream UI
  arrives unscaled: `python3 tools/scale_gui.py --check src/gui/*.c` lists bare
  pixel literals in layout calls, `--write` wraps them, and the `#define`d sizes
  at the top of each page have to be wrapped by hand. Counts, milliseconds and
  percentages must not be wrapped.
- New icons need `icons_small.c` regenerated: run the `sonix-icons:latest` image
  (`python:3.12` + cairosvg + pillow, `tools/svg_to_lvgl.py --small-only`) with
  the tree mounted at its absolute path and `-u $(id -u):$(id -g)`. Do not
  regenerate `icons.c` -- it is upstream's and not compiled here.
- Language files: merge `assets/R3PII/.../language/*.ini` into
  `assets/BLAZE/.../language/` three-way (base = previous R3PII file, ours =
  BLAZE). Ours carries the V3 AK4493 strings, theirs the new keys.
- Upstream's boot-logo marker, its charging screen and its kernel modules do not
  apply here; the guards for the first two are in `bootlogo.c`
  (`bootlogo_board_supports()`) and in the AXP2101 read that fails at once.
- `SC()` wraps pixels only. A number in another space must not be wrapped:
  `topbar.c`'s `BATTERY_CAVITY_*` are 24x24 SVG coordinates that
  `draw_battery()` scales by `BATTERY_ICON_SIZE / 24` itself, and scaling them
  here shrank the level bar to two thirds of the cavity and shifted it. The same
  goes for percentages, blur radii in a 0..255 space, and counts.
- `SONIX_BATTERY_CAPACITY` / `SONIX_BATTERY_STATUS` take *paths* to files, not
  values: `printf '64\n' > /tmp/cap`, `printf 'Discharging\n' > /tmp/stat`. That
  is how the battery icon is checked in the simulator at a level, low and
  charging.
- Language files: a 3-way merge keeps "deleted on our side", so a key our old
  BLAZE file lacked stays missing while upstream has it, and the player shows
  the key itself on screen. After every merge, diff each `.ini`'s key set
  against upstream's and count duplicates (`uniq -d`); `docs/UPDATING.md`,
  step 5, has the loop. The loader keeps the **last** line of a key.

## Updating the player on the device: / and /tmp are not the same filesystem

`/` is a 52 MB UBIFS volume and it boots **100 % full** (the packer fills it).
`/tmp` and `/data` are not: `/tmp` is tmpfs, `/data` is UBIFS with ~36 MB free.
So `mv /tmp/sonix_player.new /usr/bin/sonix_player` is not a rename -- it is a
copy that needs the whole 7.7 MB free on `/` while the old binary is still
there, and it dies with `No space left on device` **after truncating the
target**. That is a player that cannot start again, on a device whose launcher
reboots when the player exits.

Safe recipe:

1. `cp sonix-player/sonix_player /data/sonix_player.new` (not `/tmp`), `chmod 775`,
   and `cp /usr/bin/sonix_player /data/sonix_player.prev` -- a rollback copy of
   the running build, which `/data` has room for;
2. free space on `/` if the copy will not fit. **Measured 2026-10-24** on a
   build made by the current packer: `/` is 52 MB with **6.2 MB free**, not the
   100 % full volume the older images were -- `trim_stock.py` already removed the
   stock decoders (`libFLAC`, `libwma`, `libmp3lame`, `libaac`, `libmp3`,
   `libdsd2pcm`, `libmad` are all gone), so that step of the old recipe is a
   no-op now. `rm` first, then copy; nothing else had to be deleted;
3. `rm /usr/bin/sonix_player`, then `cp /data/sonix_player.new /usr/bin/sonix_player`
   (UBIFS compresses it: 6.2 MB of free space took the 7.7 MB file and left
   1.6 MB), `chmod 775`, `md5sum` against the host build, `sync`, reboot.

A reflash restores the deleted libraries; the shipped `.upt` never had them
removed.

## Logs for a device test

Settings live in `/data/device_config.ini` and can be hand-edited (read back at
start). `[system] log_to_sd = 1` writes `/mnt/sd_0/.local/sonix_player.log`,
which is where the audio path prints its evidence: `set output to N`,
`PCM open on 'hw:…'`, `audio: buffer … frames (… ms), period … frames (… ms)`,
`jack changed, rerouting`. The kernel side of the optical route shows up in the
same file as `kernel: <4> … spdif_optical power on` / `OPTICAL_MOODE`.

`tools/keyinj.c` built statically (`mipsel-rockbox-linux-gnu-gcc -static -O2`)
drives the buttons from adb: `keyinj /dev/input/event1 116 164` wakes the screen
and presses play/pause. Push it to `/data`, not `/tmp`.

## The rootfs is exactly the volume's size, and it boots 100 % full

`mkfs.ubifs -c 480` is the whole volume, so the stock tree fills it. Anything
that adds files to the image without removing some (assets, a bigger player)
fails at the packer's `mkfs` or, worse, leaves no room to replace a binary on a
test unit. The packer therefore trims the stock libraries nothing uses
(`sonix-packer/tools/trim_stock.py`, run after `install_tree.sh`): the deleted
`hiby_player`'s decoders and the stock glib 2.40, 7.45 MB.

What must NOT be trimmed, because the rest of the firmware uses it:

- `usr/lib/libssl.so.1.0.0` -- `sbin/wpa_supplicant`, `bin/sshd`,
  `sbin/shairport` link against it, and there is **no** copy in `/lib`. Remove it
  and Wi-Fi never comes up (measured the hard way on 2026-10-24: the port is
  fine, `wpa_supplicant` is simply not running).
- `lib/libcrypto.so.1.0.0` -- the only copy; the duplicate in `usr/lib` may go.

The trim script checks the finished tree for ELF files that still name a
removed soname and fails the build if it finds one, so a wrong entry is caught
at pack time rather than after a flash.

## adb and the power cable are the same cable

One USB-C port carries both, so a measurement of the device **on battery** is
blind: unplugging the cable takes `adb` with it, and there is no shell, no
`dumpsys`-equivalent, nothing -- until the cable is back. Consequences that keep
biting:

- a discharge or standby-rate measurement has to write its data to `/data` (not
  `/tmp`, and not a `adb shell` pipe) and be read after the cable returns;
  `tools/powsample` (`powsample.c`, documented in `docs/POWER.md`) exists for
  exactly that: one CSV line per interval with the battery current, the level,
  the backlight, **both panel rails** and the `slcd_vsync` count;
- with the cable in, `suspend_if_idle()` refuses `mem` while **ADB is running**
  (`power: mem deferred: ADB is on with the cable plugged in`) -- by design, so
  a session does not die mid-test. Testing the "asleep on the charger" case
  therefore means turning ADB off in Developer options first, which ends the
  adb session;
- a plain charger with ADB off no longer blocks `mem` (changed 2026-10-24): the
  charge limit cannot be enforced on this board (no MP2731 node), so there was
  nothing to stay awake for. A cable carrying the card to a host, DAC mode or
  ADB still does.

## Where the player's own log goes

`[system] log_to_sd = 1` in `/data/device_config.ini` writes
`/mnt/sd_0/.local/sonix_player.log`; the launcher sends stdout and stderr to
`/dev/null` otherwise (`/proc/<pid>/fd/1 -> /dev/null`), so with the key off
there is no player log at all. The power path's lines are the ones to read for
a sleep test: `power: initialized (...)`, `power: screen off`, `power: wake
serial (us): before … unblank … settle … fb_begin … draw …`, `power: mem:
writing`, `power: mem: awake after N s`, `power: mem deferred: <reason>`.

## The device clock is not the host clock

Measured 2026-10-24: the host was at 12:20 MSK while the device reported
12:19 **CST** -- the same instant, but the device's timezone is UTC+8, i.e. five
hours behind the owner's wall clock (and its log timestamps are the device's
own). Do not correlate device log lines with host times; correlate them with
each other. The RTC alarm math is unaffected (it works in epoch seconds), but a
"12 h battery check" reads as a different hour than the one on the owner's
clock.

## The battery current sensor is not usable on this firmware

`/sys/class/power_supply/axp_battery/current_now` reports **200000 (uA) both
while charging and while discharging** -- it is the configured charge current,
not a measurement. The CW2015 gauge (`/sys/class/power_supply/battery`) exposes
`voltage_now`, `capacity`, `status` and `temp` only, no current. So a discharge
measurement on this device is limited to:

- the power **state** (rails, backlight, `slcd_vsync`, `/proc/stat`, per-thread
  ticks) -- `tools/powsample` records exactly these;
- `voltage_now`, which drifts ~10 mV per few minutes at idle, too coarse for a
  short window;
- `capacity`, whole percent, i.e. ~16 mAh with the 1600 mAh cell.

An honest mA number would need a meter in series with the battery, i.e. opening
the device. Do not promise one from adb.

## Telling a hard power-off from a reboot on the device

A player that "went off" is not necessarily a crash, and the difference decides where to look:

- `cat /sys/class/axp/axp_reg` after `echo 0x20 > ...` gives the power-on status. `0x01` means
  the **key** started it, i.e. the device was really off (a reboot by the launcher or a
  watchdog would show no key and a shorter uptime gap);
- a hard power-off leaves **no closing lines** in `/mnt/sd_0/.local/sonix_player.log` -- the
  last line is whatever ran before the supply went, so an event "at the same time" as the last
  line is not in the log at all;
- `uptime` plus the log's last timestamp separates "the device rebooted 10 minutes ago" from
  "the player restarted inside a running system" (the log then shows a new
  `power: initialized` line, the device keeps its uptime).

Measured on 2026-10-24: inserting a 4.4 mm plug killed the supply twice, and the log simply
stops.

## 4.4 mm plug insertion can cut the power (open, 2026-10-24)

Reproduced 3 times in one session, always on inserting the 4.4 mm Pentaconn plug: the device
loses power instantly (no reboot -- `REG[0x20]` afterwards says the power key started it, and
it must be switched on by hand), or the picture freezes when the player is killed by the
kernel's SIGHUP after the plug event. The **stock firmware does not do it**: the same plug,
3-4 insertions, clean (flashed `sonix-packer/v3_analog_original.upt`, md5s `35889275…` /
`1f54858d…`, then flashed the port back). It is not the cable and not the pack's protection.

Clean insertions happen too (5+ in the same session), so it is a race. Everything below was
ruled out: charge current held at 0 and charge voltage lowered to 4.0 V both still died, so it
is not the AXP BATFET opening; the pack sits at 4.4 V on stock as well (the driver sets
`REG[0x64]=0x5` at boot and the stock player never writes it).

**Catch it live.** The player's own log is useless for the last second: with `log_to_sd = 1`
the port pipes stdout/stderr through a thread that `fdatasync`s at most once a second
(`QUEUE_SYNC_MS` in `logging.c`), and a hard power cut keeps whatever was not synced. So:

```
# stop the supervisor first, or it reboots the device when the player exits
kill $(pidof sonix_launch); killall sonix_player
# SONIX_LOG makes the player write its log straight into that file (no relay thread)
adb shell 'sh -c "ulimit -s 1024; export MALLOC_ARENA_MAX=1 SONIX_LOG=/data/pl.log; \
    cd /; exec setsid /usr/bin/sonix_player" </dev/null >/dev/null 2>&1 &'
# and keep the flash caught up, so the tail survives the cut
adb push tools/...  # /data/syncloop.sh: while true; do sync; usleep 200000; done
adb shell 'setsid /data/syncloop.sh </dev/null >/dev/null 2>&1 &'
```

`setsid` is not optional either way: a player started as an ordinary background child of an
`adb shell` is killed by the kernel's SIGHUP when that shell exits, and the crash handler
records it as `*** sonix_player is being stopped: SIGHUP *** *** sent by pid 0 uid 0 ***` --
which reads like a bug and is not one. A plain reboot restores the normal setup (the init
script starts `sonix_launch` again).

**The diagnosis trial also failed to reproduce it**, which is itself a result: with the
player in a manual session, `SONIX_LOG` on `/data`, a 200 ms `sync` loop, the charge current
held at 0, music playing through 4.4 mm and the screen dark -- i.e. every condition of the
three failures -- four insertions went through cleanly. So the charge hold is **not** the
trigger, and neither is the state the device was in when it died. What the failures do share
is a **time window**: all three fell inside ~40 minutes (13:45-14:25), with a dozen clean
insertions before and after. That points at something that comes and goes (temperature, the
pack at 4.41 V settling, a stale driver state after the day's many reboots) rather than at the
plug.

The device was returned to the normal setup (`log_to_sd = 1`, `sonix_launch` supervising) so
that the next occurrence leaves the card log. Expect the last second to be missing from it.

What to look for when it happens: the last `set output to`, `jack changed`,
`PCM open`, and the `kernel:` lines the port relays (the codec's `hbc2500_hw_enable`,
`hbc3000 id code`, and `i2c i2c-1: transfer error, ABORT`). The I2C aborts appear on clean
insertions too, so they are only a lead when a *new* kind of line shows up next to them.
