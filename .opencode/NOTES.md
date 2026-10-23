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

1. `cp sonix-player/sonix_player /data/sonix_player.new` (not `/tmp`), `chmod 775`;
2. free ~7.7 MB on `/` -- the otherwise unused stock decoder libraries are the
   cheapest: `libFLAC.so{,.8}`, `libwma.so`, `libmp3lame.so{,.0}`, `libaac.so`,
   `libmp3.so`, `libdsd2pcm.so`, `libmad.so.0` (~4.1 MB), and if still short
   `libcrypto.so.1.0.0` (1.9 MB), `libssl.so.1.0.0` (0.38 MB),
   `libglib-2.0.so.0.4000.0` (1.36 MB) -- all stock-only, none of them is on the
   port's NEEDED list (ours are `*.so.1.1` and glib `0.4600.1`);
3. `rm /usr/bin/sonix_player`, then `cp /data/sonix_player.new /usr/bin/sonix_player`
   (UBIFS compresses it, so ~6.3 MB of free space is enough for the 7.4 MB file),
   `chmod 775`, `md5sum` against the host build, `sync`, reboot.

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
