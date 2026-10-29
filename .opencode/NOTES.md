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
