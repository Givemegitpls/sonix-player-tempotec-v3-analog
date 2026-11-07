# Project hints

Read this before building anything. It exists because one session spent three hours
rebuilding a toolchain that was already on disk.

## Where the build artifacts actually live

A fresh clone has no build outputs: everything heavy is gitignored and lives inside
the tree, built on first use.

- `sonix-player/rockbox-toolchain/` — the MIPS toolchain (binutils 2.38, gcc 9.5.0,
  glibc 2.27 `--enable-oldest-abi=2.16`), 350 MB;
- `sonix-player/freetype-target/` and `sonix-player/audio-target/` — the static
  libraries the player links;
- `sonix-player/build_target/`, `sonix-player/sonix_player`, `sonix-player/sonix_launch`;
- `sonix-packer/out/*.upt` — previously built images.

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

This tree carries its own `sonix-player/rockbox-toolchain` (350 MB),
`freetype-target/`, `audio-target/`, `build_target/` and
`sonix-packer/v3_analog_original.upt`, so it builds and packs without borrowing
anything from outside.

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

## SSH on the device: present in the image, started by nothing

`/bin/sshd` (OpenSSH_7.1p1, MIPS32r2) ships in the stock rootfs together with
`/etc/ssh/sshd_config`, host keys and `usr/libexec/sftp-server`; no `etc/init.d` script starts it.
It runs as root with `-o UsePrivilegeSeparation=no` (there is no `/var/empty`, and the rootfs is
full, so creating one is not free). Password login needs
`-o PermitRootLogin=yes -o PermitEmptyPasswords=yes`: `/etc/shadow` has `root::`, and the default
`PermitRootLogin prohibit-password` refuses it. `setsid` on this BusyBox is
`setsid [-c] PROG` -- getopt permutes, so `setsid /bin/sshd -p 22` is parsed as *setsid* options and
fails; sshd daemonizes on its own, so call it directly.

Transport without a cable: Wi-Fi (`wlan0`, `wpa_supplicant` from `/usr/bin/wifi_on.sh`) or
`adb forward tcp:2222 tcp:22`. Wi-Fi power save defaults to **on** and that is what makes the link
look broken: RTT 45-210 ms and whole pings lost at -38 dBm; `iw dev wlan0 set power_save off` takes
it to 6-9 ms. Do not conclude "Wi-Fi flaky" before checking that.

The port's own power policy will kill an SSH session: `wifi_wanted()` in `power.c` does not know
about sshd, so ~60 s after the screen goes dark and the radios fall idle, `park_wifi_now()` runs
`wifi_set_enabled_transient(false)`. Something in `wifi_in_use()` (Wi-Fi transfer, DLNA, AirPlay,
SonixLink, a stream, a cache download) has to be on to keep the link.

## Two battery gauges, and the player reads the wrong one

`/sys/class/power_supply/axp_battery` is the AXP2101's own fuel gauge (i2c 0x34, SOC in REG[0xa4]),
`/sys/class/power_supply/battery` is the CW2015 (i2c 0x62, `capacity` + `voltage_now` only).
`scan_power_supplies()` (`src/system/device/system.c`) takes the first `type == Battery` node that
reports a capacity above zero, and readdir order puts `axp_battery` first -- so the icon shows the
AXP SOC. Measured 2026-10-25: AXP says 100 % (REG[0xa4]=0x64) at VBAT 4.129 V while the CW2015 says
87 %. The AXP's model is the vendor's fixed `axp2101_model[128]` table in `vendor-drivers/axp2101.c`
and the board charges to 4.40 V (REG[0x64]=0x5) against a 4350 mV design maximum; the chip's own
time-to-empty reads 83 min (REG[0xa6..0xa7]=0x0053) while claiming 100 %, i.e. the model is not this
pack's. The real level for anything battery-related is `cat /sys/class/power_supply/battery/capacity`.

Fixed 2026-10-25: the gauge is named on the board instead of guessed -- `battery_supply`
in `sysinfo_model_t` (`board.h`, `"battery"` on this board), honoured first in
`scan_power_supplies()` and even at 0 %, with the old walk as the fallback for boards that
name none. Charging status is still taken from every battery node, so `axp_battery` keeps
reporting `Charging`.

`printk` timestamps stop during suspend while `/proc/uptime` does not, so the gap between the last
`dmesg` timestamp and `/proc/uptime` is the accumulated `mem` time -- the cheap way to tell whether
the device actually slept.

## The player dies outright with the panel dark (open, 2026-10-25)

Symptom, in the owner's words: the panel is off, music plays, the music stops abruptly, the power
key does nothing, the LED stays lit (yellow); only a long hold of the power button and a fresh
start bring it back. Twice today: `16:40:32` and `18:44:59`, both while playing with the panel
dark, the second one after a track change.

What the log shows and what it does not. The player's log ends on an ordinary ten-second audio
statistics line; there is no `clock: writing ... before shutdown`, no `kernel: the kernel failed
under the player` (the oops path in `logging.c` would have caught a die() report), no
`watchdog: UI thread has not run`. So nothing in the player noticed: the whole SoC stops, and
`kernel/panic` = 3 plus no pstore/ramoops/`last_kmsg` means a reset leaves no trace at all.

The one stall the player did catch, `14:01:38` the same day, names the mechanism worth chasing:

```
watchdog: UI thread has not run for 5 s ... tid 152 (card supervisor) state=D wchan=sleep_on_buffer
                                          tid 191 (queue writer)     state=D wchan=sleep_on_page
watchdog: UI recovered after ~8 s
jzmmc_v1.2.0: timeout 3000ms op:25 w sz:524288 ... blks:20/1024
```

Writes to `mmcblk0` do not complete inside the driver's 3 s timeout -- not even 13 KB ones -- so the
task ends up uninterruptible on the card, and everything that waits on it (the UI included) stops.
Free memory sits at **0.8-1.0 MB of 57 MB** (`Mlocked` 7.9 MB, `Active(anon)` 17 MB), so almost
every allocation needs direct reclaim, and reclaim of file pages has to go through that same card.
That is the working hypothesis: a card that stalls under writeback, at a free-memory level with no
slack, hangs the kernel somewhere that prints nothing.

The card itself is not healthy: at the `18:45` boot the FAT layer said `Volume was not properly
unmounted`, then `fat_free_clusters: deleting FAT entry beyond EOF`, then
`Filesystem has been set read-only` nine seconds in. With the filesystem read-only the player keeps
writing its log into the page cache: the file reads up to the current second while its mtime and
size stop at the moment of the remount, and a power cycle takes all of it. **Pull the log before
touching the power button** -- that is how the `18:44` tail was saved; otherwise it is gone with the
cache.

Diagnostics now in place (2026-10-25), all on `/data` because the card cannot be trusted with them:

- `/etc/init.d/S92_03_start_music_player` is hand-patched (stock copy next to it as `.stock`):
  `SONIX_LOG=/data/sonix_player.log` before the player starts, and `/data/diag_start.sh` first.
- `/data/diag_start.sh` rotates `probe.log` / `sonix_player.log` of the run that just ended into
  `*.prev.log` (the player opens `SONIX_LOG` with `"we"`, i.e. truncating, so without the rotation a
  hard reset would cost the previous boot's tail) and starts the probe.
- `/data/diag_probe.sh` samples once a second: MemFree/Cached/Slab/Dirty, `allocstall`,
  `/sys/block/mmcblk0/stat`, load, and once a minute every player thread with its state and wchan
  plus the mmc interrupt counters. It tails itself to 4 MB and refuses to start twice.
- `/data/diag_capture.sh` is for the moment it hangs: per-thread stacks, meminfo, vmstat, mmc and
  interrupt counters, dmesg, then `echo w >/proc/sysrq-trigger` (sysrq is 1 on this build,
  `CONFIG_STACKTRACE` works, so `/proc/<pid>/task/*/stack` is readable). Run it **before** the
  power button: `adb shell /data/diag_capture.sh`.

Two things not to conclude. `panel_blank_now()`/`suspend_if_idle()` are not the trigger while music
plays: `power.c` keeps `g_mem_active_ms` fresh on every tick with `playing`, so the idle path never
runs, and no such suspend is in the log either. And the `i2c-1` `ADDR_NOACK` bursts (AK4493 + sau)
are the DAC being unpowered at boot and after resume, not a fault.

### Three traps met while putting that in place (same day)

1. **A backup in `/etc/init.d` is a boot script.** `rcS` runs `/etc/init.d/S??*`, so
   `S92_03_start_music_player.stock` -- the copy made before hand-patching -- matched the glob and ran
   too. Two `sonix_launch`, two `sonix_player`, one environment: the second instance could not open
   `hw:1,0` (the first held it), fell back to `hw:0,0`, wrote `database is locked` into the library,
   and drew into the same framebuffer as the first -- on the glass that reads as the interface
   coming apart. The stock copy now lives in `/data/init_backup/`. Nothing under `/etc/init.d` may
   start with `S` unless it is meant to run.
2. **A sampler must not `sync`.** The first version of the probe synced every ten seconds to make
   its own lines durable. That dragged every dirty page of the system -- the library database's
   included -- out to the SD card on the probe's rhythm: the card went to 78% busy
   (`ms_writing` 287 s over 370 s of uptime, 266 ms per write), `jzmmc` started timing out at 3 s,
   the player printed `database is locked` and tracks aborted on open. With the sync removed the
   same counters read 7 ms per write. A few hundred bytes on ubifs is nothing; ubifs commits on its
   own, and the last seconds before a hang are what `/proc` state is for.
3. **A pid file survives a reboot and pids get reused.** The probe's "already running" guard kept
   the number from the previous boot, found it taken after the restart, and stayed off for the whole
   boot. The guard now checks that `/proc/<pid>/cmdline` is really another probe.

Also: `diag_start.sh` rotates the live log, so it must only move files with the player down --
run by hand while music plays it takes `sonix_player.log` out of the namespace and the player keeps
writing into a deleted inode.

State after the fixes: one `sonix_player`, audio on `hw:1,0`, card writes ~7 ms each, the probe
running without `sync`, the player's log on `/data/sonix_player.log`.

## What the sound card actually receives (measured 2026-10-26)

`tools/pcmtap.c` (preload, dumps the PCM frames a player hands the card) settled
what no log could:

- Both firmware players deliver the file **bit-identically** on the jack route:
  the port through `hw:1,0`, the stock through `pcmC1D0p` (4.4 mm plugged). No
  resample, no dither, no attenuation.
- The port is bit-identical on the **optical** route too (`hw:0,0`, nothing
  plugged). The stock is not: its card-0 node (`SA SPDIF Dummy`) takes
  uniformly-random bytes -- not audio -- and the audible path there goes through
  the vendor `/dev/sa_hgl_dma`, which takes no `write()`, no `mmap` and no
  per-period ioctl (its buffer is filled by plain stores). Plug a jack and the
  stock is ordinary again on card 1.
- The stock player never uses alsa-lib for playback: it opens
  `/dev/snd/pcmC%dD%dp` itself (`openat64`) and drives it with ioctls, so a
  preload must hook `ioctl` (`type == 'A'`, `nr 0x50` = WRITEI_FRAMES) -- hooking
  `snd_pcm_writei` alone sees nothing on stock.
- Volume never appears in the samples on the jack route (the AK4493 holds the
  level), so a capture at whatever volume was set is still the file. The port's
  `swvolume` runs on the USB-C port only.
- Frame size is `period_bytes / period_size`, never the format mask: on this
  kernel the mask reports the same bit for a 16-bit and a 24-bit stream, and
  `/proc/asound` has no `hw_params`. `struct snd_pcm_hw_params` is **604** bytes
  here -- `snd_interval` packs flags into bit fields, so intervals are 12 bytes.
- The stock `hiby_player.sh` reboots the device when the player exits: kill the
  `.sh` (pid of the wrapper) before killing the player. The `azbar` image is
  stock + `/etc/init.d/S98adb_debug`, which only keeps adb up while
  `/mnt/sd_0/enable_adb` exists.

## Three facts that cost an afternoon each

- **The kernel has no inotify.** `inotify_init()` and `inotify_init1()` both answer `ENOSYS` on the
  device; `/proc/sys/fs/inotify` does not exist (`CONFIG_INOTIFY_USER` is off). Anything that waits
  for a file to appear has to poll, so make it a `stat`/`access` of one name and never an `open` of
  the card. Measured 2026-10-25 with a throwaway probe.
- **`SET_SLEEP` does not touch the backlight class.** The vendor panel power path gates the LCD
  clocks and the rails and leaves `/sys/class/backlight/.../brightness` exactly where it was, so
  every place that powers the panel down -- `power_screen_off()`, `chargescreen`'s `dark()` -- has to
  write 0 itself. And `sysinfo_model_t.backlight_dir` is the **whole** path
  (`/sys/class/backlight/pwm-backlight.0`), as `main.c` composes its nodes; appending it to
  `/sys/class/backlight/` a second time fails silently and the screen just stays dark.
- **LVGL's tick is cached for a whole `lv_timer_handler()` pass** (main.c), so `lv_tick_elaps()` is
  always 0 inside a timer callback. Any cooperative slice ("work until N ms") has to use
  `ui_tick_ms_raw()` from `system/core/utils.h`, or it never yields.
- **The log lives in a queue in the player, so `sync(2)` does not save it.** `logging.c` hands
  printed bytes to a 64 KB ring and a `logwriter` thread writes and `fdatasync`s them; the shutdown,
  restart and `mem` paths call `logging_flush()` (bounded 1.5 s) for that reason. `logging_sync()`
  is the watchdog's write-through and costs nothing when nothing was printed.
- **The log file is reopened when the path's inode changes** (`sync_card()` →
  `log_reopen_if_replaced()`), so rotating it with the player up is safe now: at most the ~1 s of
  lines written before the notice goes into the old, renamed file. Replacing it more often than
  that will still lose those lines.
- **A 56 MB `adb push` to the card can wedge `jzmmc`**: `timeout 3000ms op:25 w sz:12288`, then
  `/dev/mmcblk0` disappears from `/proc/partitions` and `/mnt/sd_0` turns into an empty directory on
  the rootfs (a push "succeeding" there would fill a 100 %-full rootfs). Stop the player first
  (`killall sonix_launch`, then `killall sonix_player`), push, verify `md5sum` on the device at once,
  and reboot to get the card probed again.
- The log's stamp is a record of its own in the queue, so a line can start without one when the file
  is replaced under it, and two threads printing at once used to interleave stamps (`line_lock` now
  keeps a line's records adjacent). Cosmetic, but it is why a rotated log may open with a bare line.

## Wake from `mem`: only the power key is a wake source (measured from the stock kernel)

`power.c` does not decide this. The stock kernel's `gpio_keys` table has `wakeup = 1` on the power
entry only, and `gpio_keys_suspend` calls `irq_set_irq_wake` only for buttons with that field set;
`gpio_suspend_set` then masks every pin not in `wake_map`, so a play press in `mem` raises no bank
IRQ at all and the event never arrives after the resume.

The table (decompressed kernel, base `0x80010000`, stride 0x2c, file offset = virt − 0x80010000):

| key | code | gpio | wakeup field |
|---|---|---|---|
| play | 164 | 99 = PD(3) | VA `0x8064cbac` |
| next | 163 | 88 = PC(24) | `0x8064cbd8` |
| prev | 165 | 60 = PB(28) | `0x8064cc04` |
| power | 116 | 63 = PB(31) | `0x8064cc30` = 1 |
| vol+ | 115 | 35 = PB(3) | `0x8064cc5c` |
| vol− | 114 | 32 = PB(0) | `0x8064cc88` |

Table starts at VA `0x8064cb98`, `gpio_keys_platform_data` at `0x8064cca0` (name
`ingenic-v3_analog`). Only one copy, one reference. Symbols: decode `kallsyms` at
`0x5739f0`/`0x588900`/`0x58890c` and pair `names[i]` with `addresses[i-4]` — without the 4-entry
shift every symbol lands on its neighbour (`jzfb_set_par` reads as `jzfb_open`).

The SoC side is ready: `setup_gpio_irq` wake-enables all four banks, so a patched `wakeup` word for
a bank-B/C/D button is enough — only the PD bank surviving `mem` with its pull-up is unverified.
`gzip -9 -n` of the patched image is 2166 bytes smaller than stock, so the packer's size limit
holds. The player needs no change: with the screen dark a play press only calls
`power_notify_activity()` and `gui_notify_key(PLAY_PAUSE)`, and `suspend_to_ram()` re-blanks the
panel after a resume with `!g_screen_on`.

## Audio chain (vendor spec sheet, 2026-10-26)

2x AK4493SEQ, one per channel (dual mono, fully separated L/R); LPF 2x OPA1652, output
buffers 4x OPA1688. 3.5 mm SE 2 Vrms / 330 mW@32R, 4.4 mm BAL 4 Vrms / 610 mW@32R,
SNR 124 dB, THD+N 0.0008 % (SE) / 0.0005 % (BAL), output impedance 0.29 R (SE) / 0.92 R (BAL).
High/low gain switch; the 3.5 mm jack is also line-out and mini-Toslink.
The firmware runs **HiBy OS** (licensed), so MSEB, HiByLink, AirPlay/DLNA and the HiBy player
DSP are the same family as on HiBy's own DAPs — the port replaces the player, not the OS stack.
Not in the docs: the clocking scheme (single XO vs per-family) is still unknown.

## The packer packs `sonix-packer/sonix_player`, not the one you just built

`SKIP_PLAYER_BUILD=1` skips `build_player()`, and `build_player()` is the only
thing that copies `sonix-player/sonix_player` next to the packer. The copy in
`sonix-packer/` is a snapshot from whenever the last full run happened, so a
`SKIP_PLAYER_BUILD=1` run after a player build silently packs the old binary
into a new rootfs (2026-10-28: `c066e453` image carried the Oct 25 player).
Run `./v3_analog_packer.sh` without the flag -- its own build is a no-op when
nothing changed -- and check that
`md5sum sonix_player ../sonix-player/sonix_player build/verify/SYSTEM/usr/bin/sonix_player`
agree before flashing.

## A scan holds the device out of `mem` (verified on the device 2026-10-29)

`suspend_if_idle()` refuses with `power: mem deferred: a scan is writing to the card` while
`library_scan_running()` or `audiobookdb_scan_running()` is true, and the check sits **before**
the cable check so a bench session with adb attached still names the scan. The standby countdown
is restarted on every tick while a scan runs (`g_mem_active_ms` in the tick), so the sixty
seconds begin when the scan ends. The owner reproduced it on the device with a full scan and the
screen dark: no `power: mem: writing` until the scan finished. The log line now reads
`power: initialized (... mem=on/60s ...)`; it said `mem=TEST` when standby was on.

## Pulling the card mid-scan: the card suffers, and so does the index (measured 2026-10-29)

Pulling the card while a scan writes cost the card's FAT (`FAT-fs (mmcblk0p1): error,
fat_get_cluster: invalid cluster chain` -> `Filesystem has been set read-only`, at the pull and
on every boot after; the device cannot repair vfat, `fsck.vfat -a` on a PC can, and it reclaimed
104 MB of orphaned clusters), a hot `library.db-journal` (truncated by that fsck; SQLite then
opens the file as it was -- 3262 rows, `PRAGMA integrity_check` ok).

On the reverted scan logic the library itself is not protected: the wipe happens before the
walk, so an interrupted run leaves only what it had committed. Do not pull the card during a
scan; if it happens, fsck the card before trusting the library, and expect a rescan.

## Flashing: verify the name recovery will read, not the one you wrote (2026-10-29)

The image must be at `<card>/v3_analog.upt`. On this card a rename may not survive the reboot that
follows the trigger: `mv v3_analog.new.upt v3_analog.upt` looked right (`ls` showed the new file
under the final name), recovery then read the **old** `v3_analog.upt`, and after the boot the two
names were back as they were — the update flashed the previous build and `build_version` in
`system-info.json` still named the old player, which is the only sign. Do `rm` of the old name,
`mv`, `sync`, then `md5sum` **of the final name** immediately before
`echo recovery > /proc/jz/reset/reset`, and compare it with the packer's output.

A 56 MB push to the card can also wedge `jzmmc` (`/mnt/sd_0` empties, `dmesg` shows traces); a
reboot brings the card back and the same push usually goes through on the second try. Stop the
player first (`killall sonix_launch; killall sonix_player`), push under a temporary name,
`md5sum` it on the device, then move it into place.

## Replacing the player on the device: `adb reboot` does not work here (2026-10-29)
`adb reboot` answers `reboot failed: Bad address` on this adbd; `adb shell 'sync; reboot'` does
reboot (uptime resets, init starts `sonix_launch` again). If the launcher was killed and the
reboot did not happen, the device stays up with no player: start it with
`adb shell 'setsid /usr/bin/sonix_player.sh </dev/null >/dev/null 2>&1 &'` — but a real reboot is
better, because a hand-started player inherits the adb shell's environment (`cwd=/`, no
`ulimit`/`MALLOC_ARENA_MAX` from the init script unless the script itself sets them).

`/data` had 25 MB free and `/` 5.7 MB free after the 2026-10-29 deploy, with `/data` otherwise
empty — the diag scripts and logs described in the older notes are not on this unit.

## The scan is upstream's, and it stays that way (2026-10-29)

The scan was reworked in this session -- a sweep instead of the wipe, per-file
`forget_file()`, folder marks that survive, logging of skips -- and then **reverted in full**
after measuring it. Kept from that work: only the `mem` protection (below).

What the code does (upstream 1.3.0, unchanged):

- `full_scan_run()` runs `DELETE FROM MEDIA_TABLE/ALBUM_TABLE/.../FOLDER_TABLE/CUE_STATE`
  **before** `scan_directory()`, then `BEGIN`, walks, `COMMIT`. A cancel, a power cut or a
  failed `COMMIT` therefore leaves the partial index as the library, and a card pulled
  mid-scan loses what was not written yet. That is the original behaviour and it is
  deliberate: the alternative cost time on every scan.
- rows are committed every 50 (`SCAN_COMMIT_EVERY`); a failed `INSERT` used to be printed and
  counted anyway and the folder was still marked, which is how a gap became permanent. The
  rework fixed that; it is gone with the rework, so the risk is back.
- no library ceiling and no `LIMIT`: "All tracks" is `SELECT rowid, sortkey FROM MEDIA_TABLE
  ORDER BY sortkey`, and the count is `COUNT(*)` of the same table.
- the folder subset filter is `scan_folders.txt` in the player's user-data dir, not on the
  card; top-level folders not ticked are skipped and loose root files are dropped. A complete
  run with a subset produces the subset index.

**Why it was reverted -- the measurement** (same card, same 3262 files, same database):

| build | scan |
|---|---|
| upstream logic (`/data/sonix_player.prev`) | 17:32:34 -> 17:34:15, **101 s** |
| rework | 17:28:22 -> 17:30:50, **148 s** |
| rework + no per-file delete in a full scan | 17:36:52 -> 17:39:0x, **~130 s** |

The per-file `forget_file()` (four `DELETE`s inside the transaction) is most of the difference;
the rest is inserting into a populated table and the 3262-row sweep at the end. The rework was
bought for a report ("400 of 13000 tracks") that could not be reproduced on this card and has no
confirmed mechanism, so the scan went back to upstream's and only the `mem` rule stayed.

**What is still worth knowing about scanning** (measured, unchanged behaviour): no per-file cap,
no depth problem at 3262 files; the card is the cost (about 31 ms per file, 25-32 tracks/s);
a scan holds the standby off (below); `dr_flac` opens FLAC in strict mode, so a file whose first
block is not STREAMINFO/34 is indexed but will not play (`decode.c`, `dr_flac.h`).

## Streaming keys: the port needs no `streaming-keys.bin` (2026-10-29)

The sealed-key machinery in `streamkeys.h:20-40` / `tools/seal_streamkeys.py` /
`Makefile:286-306` is inherited from the Blaze design; this port never shipped a
`.bin` and does not need one:

- **Qobuz** derives its application id and secret at run time from
  `play.qobuz.com` (`qobuzkey.h`, `qobuzkey.c`) and caches them in
  `<card>/sonix/streaming-session.json`. The file is ours: it arrived in
  `db3cd6d` and does not exist upstream (`raw.githubusercontent.com/.../qobuzkey.c`
  answers 404).
- **Tidal** carries the public tidalapi client pair, base64-wrapped in
  `tidal.c:99-105`.
- Only **Podcast Index** and **Last.fm** still read `streamkeys_*`, and they also
  accept a plain `streaming-keys.ini` beside where the `.bin` would be
  (`streamkeys.h:40`). Without it those two services disable themselves and say
  so.

So "no `streaming-keys.bin`" disables two optional services, not streaming.
`README.md:196-208` and `docs/BUILD.md` §7 already say this correctly.

## Never commit the patched kernel (2026-10-29)

`patch/v3-panel-blank/patch_kernel.py` writes a patched **HiBy** kernel image. HiBy never
published that kernel's source, so the output is their code with our bytes in it and must stay
out of git (`patch/v3-panel-blank/UIMAGE.BIN` is in `.gitignore`, and
`sonix-packer/kernel/UIMAGE.BIN` always was). Build it locally from the vendor `.upt`; the md5s
of the current output are recorded in that directory's README and in `docs/KERNEL.md`. The
upstream project's `sonix-packer/kernel/xImage-R1/R3PII` are different: those are built from
published sources, so they stay tracked.

## `one_boot_logo` is inert on this tree

`sysinfo_model_t.one_boot_logo` is declared (sysinfo.h:100) and set false
(board.h:86) but read by nothing; the only gate of the whole boot-logo path is
`boot_logo_flash` (`bootlogo_board_supports()`, bootlogo.c:57-60). README.md:95
cites `one_boot_logo` as the reason the port does not write the logo, and says
the picture comes from a `logo` partition, which no table in the repo shows --
V3-ANALOG.md contradicts itself on the source (`:522` bootloader, `:676` code
comment says kernel/initramfs).

## Wi-Fi transfer: the stock CGIs check a path by prefix (2026-10-30)

Read-only listing worked and every write did not: `upload.cgi`, `create.cgi`,
`move.cgi` and `delete.cgi` compare the path they are sent against four strings
compiled into them -- `/mnt/sd_0/`, `mnt/sd_1/`, `/mnt/udisk_0/`,
`mnt/udisk_1/` (`.data 0x41a400` holds pointers to `.rodata 0x409f00` and
friends) -- with a function that is `for each s: if (strcmp(s, path) <= 0)
return 1`. That is a prefix test, not a resolution: `/data/mnt/sd_0/...` sorts
after all four and is rejected, and `/var/log/cgic.log` says so
(`(W) DELETE: Invalid path /data/mnt/sd_0/FSCK0000.REC`). `download.cgi` checks
nothing, and `sonix-list` is ours, which is why reads looked fine.

So the page speaks `/mnt/sd_0/` (`web/index.html`, `CARD`), `wifitransfer.c`'s
`CGI_CARD_ROOT` is `/mnt/sd_0` so the bind mount lands on a name they accept,
and `webcgi.c` accepts the four `/mnt` names plus `/data/mnt` for an older page.
Permissions, ownership and the mount flags were never the problem: the card is
`rw` and both the player and thttpd run as root.

`webpage.h` and `web/build/index.html` are generated: after any `web/` change
run `python3 sonix-player/tools/web_to_c.py` or the binary keeps serving the old
page.

## medialist row windowing must go through row_at_y()

`src/gui/library/medialist.c` binds a pool of 12 rows over the viewport. The
band is derived from the scroll offset, and on one album's tracks the rows sit
`head_height` below the top of the page (plus a `DISC_HEAD_PITCH` per disc
heading), so `scroll / ROW_PITCH` overshoots by up to five rows: the rows the
viewport opens on were bound to nothing and hidden, and a scrolled album showed
its tracks only on the lower half of the screen. `band_first()`,
`window_update_impl()`'s `visible_first/last` and `rest_cb()`'s `visible_last`
all now use `row_at_y()`, the inverse of `row_y()`. Any new code that turns a
scroll offset into a row index has to do the same.

Reproducing it needs care: `band_first()` clamps to `count - ROW_POOL`, so an
album of 12 tracks or fewer (and any list scrolled to its very end) binds every
row whatever the arithmetic says. Use a record with more than 12 tracks and a
small drag that lands mid-list.

The host simulator only indexes the card if `/mnt/sd_0` exists and is writable:
without it the log says `storage: /mnt/sd_0 is READ-ONLY`, the library holds 0
tracks and every list is empty. `SONIX_SD_ROOT` alone is not enough.

Killing a simulator by name: `pgrep -f 'sonix_player_hos[t]$'` also matches the
shell whose command line *ends* with the binary name (e.g. a trailing
`ls -l sonix_player_host`), so the kill takes the shell with it. Keep the
pattern and the rest of the command line apart.

## AirPlay: the stock binaries are in /sbin on this device (2026-11-02)

`airplay.c` used to answer `airplay_available()` from three hardcoded `/usr/bin` paths.
The V3's rootfs carries `shairport`, `shairport_on.sh` and `shairport_off.sh` in **/sbin**,
and `/usr/bin` has only `shairport_on.sh` -- put there by the shared BLAZE assets, with no
`shairport` beside it. So the page said "not available on this firmware" on a device that has
the whole mechanism, because `sys_server` starts it from `/sbin/shairport_on.sh` regardless of
what the player thinks. The check now probes `/sbin` first, then `/usr/bin`, per file.

With that fixed the receiver comes up as the stock one does: `shairport -a TempoTec%V3 -o ot
-M /tmp -b 160`, both `/var/run/airplay_*` sockets created by the player before the request,
and the advertisement on the LAN is `B7B0B0B9E889@TempoTec V3._raop._tcp.local` (checked with a
hand-rolled mDNS query: PTR `_raop._tcp.local`, A `buildroot.local -> device ip`). It is
**RAOP only** -- no `_airplay._tcp`, so this is legacy AirPlay audio, not AirPlay 2.

## DLNA: dmrd's sockets live where its working directory is (2026-11-02)

`dmrd` opens `dmr_streamer` and `dmr_control` **relative to its cwd** -- bare names in the
binary, no `/data` prefix -- and `sys_server`, started from init without a `cd`, leaves it in
`/`. So on this device the renderer's side of the pair is `/dmr_control` and `/dmr_streamer`,
while `dlna.c` bound and connected to `/data/dmr_*`. The result was a renderer that appears in
the network and plays nothing: dmrd never connects to a listener at `/data/dmr_streamer`, and
the `exit` sent on switch-off goes into a socket nobody reads, so `dmrd` processes accumulate
(three were running). `dlna.c` now binds a listener in **both** `/data` and `/` and sends
control messages to whichever accepts; verified on the device -- both sockets present, one
`dmrd`, casting plays.

## Building on this machine when the docker images cannot be pulled (2026-11-02)

`tempotec-player-build:latest` here is **Debian 12, glibc 2.36**, while the MIPS toolchain in
`~/Projects/own/tempotec/tempotec-v3/toolchain/rockbox-toolchain` is linked against glibc 2.38,
so every compile in that image dies with `version GLIBC_2.38 not found`. Rebuilding the image
from `sonix-player/docker/Dockerfile` needs `ubuntu:24.04`, which is not local and cannot be
pulled here. Two things make the host build work instead:

- `ROCKBOX_TOOLCHAIN_DIR=<abs path> make target -j<n>` in `sonix-player/` (host glibc 2.44); the
  tree's `rockbox-toolchain` is a **symlink outside the repository**, so a container would not
  see it unless that path is mounted too.
- `-v path:path:ro` on this docker (29.9.2) produced a container where the path does not exist;
  `--mount type=bind,src=...,dst=...,readonly` works. Worth reaching for `--mount` when a mount
  seems to vanish.

Packing is unaffected: `SKIP_PLAYER_BUILD=1 ./v3_analog_packer.sh` uses the local
`tempotec-fw-tools:latest` and touches no network.

## Verifying AirPlay without an Apple device (2026-11-02)

`pyatv`/`atvremote` is no help: it discovers `_airplay._tcp` only, and this receiver is RAOP-only
(`_raop._tcp`, advertised as `B7B0B0B9E889@TempoTec V3`), so `scan` finds nothing even with
`--scan-hosts <ip> --scan-protocols raop`, and `--protocol` accepts only `dmap`/`mrp`.

What does work is feeding the player's own half: the AirPlay side of the port takes raw
44.1 kHz/16-bit/stereo PCM on `/var/run/airplay_socket_server` and one message per connection on
`/var/run/airplay_meta_server` (`AIRPLAY:CONNECT`, `AIRPLAY:TITLE:9:Test tone`, `AIRPLAY:ARTIST:...`;
the number is the payload length). A ~20-line static MIPS client (`socket/connect/write` from
stdin, built with the tree's `mipsel-rockbox-linux-gnu-gcc -static`) plus
`ffmpeg ... -f s16le -ac 2 -ar 44100` is enough, and the tone is audible on the DAC -- verified
2026-11-02.

Two traps. `pcm_worker()` accepts **one** client and keeps it for the whole session, which is
shairport's own behaviour ("it connects once, in init()"); a second writer sits in the backlog and
blocks as soon as the socket buffer fills, which looks like the player hanging. So `killall
shairport` first, then inject -- and afterwards toggle AirPlay off/on to get `shairport` back under
`sys_server`'s control. And `nc` is not in the rootfs, and a shell redirection onto a UNIX socket
path does not work anyway (`open()` on a socket gives ENXIO), hence the small C client.

## /proc/meminfo on this kernel has no MemAvailable (2026-11-02)

The stock kernel is 3.10.14, and `MemAvailable` is a 3.14 addition: the line simply is not
there. Every reader that parsed only that line therefore got 0 or -1 -- `cover: 0 KB free` in the
scan log, `MemAvailable -1 kB` in the audio health line, an empty RAM card on the processes
page -- numbers that read like a device in trouble and were only a missing key.

`src/system/device/meminfo.{c,h}` now holds the one parser, with two entry points on purpose:
`meminfo_kb(key)` is the honest single value (what a *sizing* decision wants, keeps its own
floor), `meminfo_available_kb()` is MemAvailable or the `MemFree + Buffers + Cached` estimate for
anything that *reports* memory. Do not hand the estimate to the cover decode budget: it is mostly
page cache, the device has 56 MB and no swap, and evicting cache under one large allocation is
how the playback thread stalls -- `cover.c` stays on `meminfo_kb("MemAvailable")` (0 here, the
2 MB floor) on purpose, and says so.

`main.c`'s two dumps used to ask for `MemAvailable:` too and never matched; they now print
`MemFree`/`Buffers`/`Cached` instead.

## Hidden networks and the resolver's addresses (2026-11-05)

Two gaps the stock page has had since the Blaze port, both now in `main`.

**A network that does not broadcast its name was unreachable, twice over.** `wifi.c` filtered
hidden scan results out of the list (`ssid_is_hidden()`, deliberately: an entry with no name has
nothing to tap), and nothing ever set `scan_ssid=1`, so even a profile created by hand could not
have associated -- wpa_supplicant only matches beacons it hears and probes for a name it was told
to probe for. `connect_direct()` now sets `scan_ssid 1` on both paths: when it creates a profile,
and when it re-selects a saved one (a profile saved before this carries `scan_ssid=0` in
`wpa_supplicant.conf`, and `save_config` writes the repair back). Harmless for a broadcasting
network, which is why it is unconditional -- the same conclusion Compas OS reached.

The UI is a row, not a list entry: "Hidden network" above the found networks opens the shared
text-entry page in `src/gui/wireless/wifinet.c` (name, then passphrase, then the ordinary
`wifi_connect()`; the passphrase page of a *visible* network still lives in `wifisettings.c`).
The scan filter stays: a hidden network already saved shows up through `list_networks` anyway,
because that path has the name the user typed.

**DNS.** `src/system/net/netdns.c` keeps `[net] dns1/dns2` in `device_config.ini` and rewrites
`/etc/resolv.conf`: the two addresses at the top, every other line copied through untouched.
That split is what makes it survive a reconnect -- udhcpc's `default.script` never truncates the
file, it only strips its own `nameserver ... # wlan0` lines and appends them again at the end, so
ours stay first and the resolver's order keeps them. `http.c` already re-reads the file when it
moves (`dns_follow_resolv_conf()`), so no restart is needed. Applied after every lease in the
connect path **and** in `wifi_init()`: a radio that comes up at boot associates on its own and
takes its lease from the init scripts, never through the connect path.

`/etc/resolv.conf` is a plain file on this rootfs (`rcS` remounts `/` rw), but it may be a symlink
into `/tmp` on other HiBy boards -- the file is written through, never replaced by rename. Under
`HOST_BUILD` the path is `/tmp/sonix/data/resolv.conf`: without that, running the simulator would
rewrite the developer's own resolver.

Strings: `tools/lang_wifi_net.py` adds the nine tags to all twelve `.ini` files under
`sonix-packer/assets/BLAZE/usr/resource/sonix/language/` (a tag with no line is displayed as the
tag itself, see `lang.h`). The translations for Čeština/Chinese/Deutsch/Español/Français/
Italiano/Japanese/Nederlands/Polski/Português/Russian went in with it; that script is the place to
fix them, and re-running it is a no-op once the keys are there.

Verified in the simulator: the row opens the entry page, the passphrase page wears the name,
an address that is not an address keeps the page up with a toast, saving writes the file and
clearing a slot falls back to the DHCP-provided address. Boot-time apply checked by seeding
`[net]` in the simulator's config: `9.9.9.9` and `1.1.1.1` land above the `# wlan0` lines.

**On the device (2026-11-05), with `tools/touchinj` driving the interface.** A hidden network
entered through the page produced `scan_ssid=1` in `/data/wpa_supplicant.conf` and a connect
through the ordinary path; a profile saved before this (`scan_ssid=0` in `get_network`) was
repaired to 1 by tapping its row. An address typed into the DNS page landed in
`/etc/resolv.conf` above the lease's own line, and `nslookup` answered from it
(`Server: 8.8.4.4`); clearing the override through the config left only the `# wlan0` line. Two
things this cost:

* **Switching to the screen that is already up is not a no-op.** The hidden-network flow asks
  two questions on one page, and the second `switch_screen(entry_screen)` left the page visible,
  focused and completely dead to touch (back arrow included). `wifinet.c` now switches only
  when `lv_screen_active()` is not that screen.
* **`settingsrow_page()` does not scroll, and the Wi-Fi page is taller than 480 px** once the
  switch, the current network, the list and a heading are on it. Two DNS rows at the bottom were
  off the glass and unreachable. The addresses are a page of their own now, opened from a single
  row placed directly under the switch.

Device quirks worth remembering: the panel reports protocol-A touches (no `ABS_MT_SLOT`, no
`ABS_X/ABS_Y`), so `touchinj` sends `TRACKING_ID`/`POSITION_X/Y`/`TOUCH_MAJOR`; a player started
from an `adb shell` dies with the shell unless it is `nohup setsid`, and a dead player looks
exactly like a live one whose touches are ignored (the framebuffer keeps the last frame); the
screen sleeps after 30 s and a tap then only wakes it, so tests want
`[power] screen_off_seconds = 0`.

## The page that said "connected" to a switched-off access point (2026-11-05)

Two defects in the Wi-Fi page, and they came from the same place: the page and `wifi.c` were
reading a cached list where they needed the supplicant's live state.

**"Connected" was a flag from the last scan, not the link.** `refresh_networks()` took `current`
from the `[CURRENT]` column of `list_networks`, and `wifisettings.c` drew its "Current network"
heading and the "Connected" line under a name from that flag. `[CURRENT]` means "the network
wpa_supplicant has selected", which it keeps while it scans for an access point that has been
switched off and drops only when it gives up, so the page went on naming the network for as long
as the radio kept looking — and it was rebuilt on scans and commands only, so even a real
disconnect would not have reached it.

`read_link()` in `wifi.c` now reads `wpa_state` and nothing else: a network is connected when it
says `COMPLETED` and its SSID is the row's. `refresh_networks()` recomputes every row's `current`
from that, `poll_status()` refreshes the list on the transition in and out of
`WIFI_STATE_CONNECTED` (a refresh forks, so transitions only), and `poll_cb()` watches the state
too (`drawn_connected`) so the page drops the section on the next poll rather than on the next
scan. `rebuild_list()` clears `current` on every row when the state is not connected, as a last
guard against a stale list.

**A forgotten network came straight back.** `read_saved_networks()` preferred
`/data/wifi_network.txt`, which `sys_server` rewrites only when asked for `WIFI:LIST_NETWORK` —
a command this player never sends. The file therefore held whatever the stock player last put
there, and a network removed with `WIFI:REMOVE` stayed in it for good, `[CURRENT]` and all.
`wpa_cli list_networks` now comes first and the file is the fallback for a firmware whose control
socket cannot be reached. The empty answer deserves its own line: `wpa_cli` prints just the
header when the list is empty (34 bytes), so `strchr(buf, '\t')` — the old test for "was there
anything" — read a successful empty reply as a failure and fell back to the file. The exit status
is what says whether there is a reply.

`do_forget()` also had two holes. It disconnected only when the *cached* flag called the network
current, and it never checked that the entry had actually gone. It now judges the link from
`read_link()`, and its outcome is the live list afterwards. Which brought the second half of
this: **`WIFI:REMOVE` is acknowledged before wpa_supplicant applies it.** 20 ms after "OK" the
entry is still in `list_networks` (61 bytes: header plus the row) and only at ~300 ms does the
list shrink to the 34-byte header, so the immediate check called the removal failed, the retry
sent a second `REMOVE`, and the refresh that followed republished the row as "Saved, out of
range", which then stayed until the next scan. `do_forget()` waits the entry out now — 250 ms
steps, 2 s ceiling, one retry — and only then refreshes.

Finally the row's tap: `LV_EVENT_CLICKED` fires on release whether or not the press was long, so
a long press opened the menu *and* ran the tap, which on a saved row is a connect: the network was
re-added under a new id a moment after "Forget network" removed it. The row is on
`LV_EVENT_SHORT_CLICKED`; a plain tap still opens the password page, a long press only the menu.

One more thing was asked for on the way: a way back to the router's own addresses without
knowing that an empty field is the answer. The DNS page has a third row -- `wifi_dns_reset`,
"Reset to the router's", a `settingsrow_action` so it carries no chevron -- which is
`netdns_set("", "")` and a toast. Measured: `[net] dns1 = 9.9.9.9` in the config and
`nameserver 9.9.9.9` at the top of `/etc/resolv.conf`; one tap, and the file is back to the
`# wlan0` lines it had (`nameserver 1.2.3.4 # wlan0`, `search lan # wlan0` both kept) with the
override gone from the config. The language tag comes from `tools/lang_wifi_net.py`, whose
`.ini` files are pushed to `/usr/resource/sonix/language/` on the device -- a player rebuilt with
a new tag otherwise shows the tag itself (`wifi_dns_reset` on the row).

**Measured on the V3.** Connected to the hidden hotspot `GT Neo2T` (profile needs `scan_ssid=1`,
it is never in the scan): the page showed "Current network → GT Neo2T → Connected". Hotspot off:
on the next poll the section was gone and the network sat in "Available networks" as "Saved, out
of range", with `wpa_state=SCANNING` and no `Connected` anywhere. Forget of a test profile:
`list_networks` empty 0.9 s after the tap, the row gone from the page without a scan, and still
gone after a reboot.

## Automatic shutdown does not run on the charger (2026-11-07)

`power_timer_cb()` held the shutdown timer at zero for a lit screen or a busy player, and the
cable was not on that list: with auto power-off on (15 min … 6 h), a player left plugged in went
off on schedule. Fixed in `power.c`: the tick restamps `g_last_active_boot_ms` while
`usb_vbus_present()`, the firing test carries `!on_charger` as a race guard, and the RTC-alarm
branch after a `mem` wake checks the cable too. The timer therefore starts counting from the
moment the cable comes out, not from the last time the screen was on. Rationale recorded in
`docs/POWER.md`, item 5. Target build (`docker/build.sh`) passes; ABI check still reports nothing
beyond glibc 2.16.

### Verified on the device, 2026-11-07

Flashed through recovery as `v3_analog.upt` (md5 `6f3c0bff7e6cc33078c4cbcb06f2290f`, build_version
`071120261246`; the previous image was kept on the card as `v3_analog_20261105.upt`). Auto power-off
set to 15 min in `/data/device_config.ini` -- note that it is the file the player reads, and that
an unknown `screen_off_seconds` (45) is normalised to the default 30 s at startup, which is how the
reading was confirmed.

- With the cable in: `15:01:00 power: screen off`, the 15-minute deadline fell at 15:16, and the
  device was still up and answering adb at 15:24, with no `automatic shutdown` line.
- With the cable out (the control): `18:13:28 power: mem: RTC wake in 846 s for the automatic
  shutdown` -> `18:27:35 power: automatic shutdown: RTC wake after mem: the idle time has run out`.
  That is the branch carrying the new `!usb_vbus_present()`, so the gate does not disable it.

adb on this port is USB only, so a cable is also VBUS: "unplugged with adb" cannot be produced, the
control has to be done by hand.
