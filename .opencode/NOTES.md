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
