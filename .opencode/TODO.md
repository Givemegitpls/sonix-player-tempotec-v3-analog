# Build pipeline — closed 2026-10-22

Goal: a person with a Linux box, **docker and nothing else installed**, clones
this repository, points it at the vendor's OFW 1.3 `.upt`, runs one script, and
gets `sonix-packer/out/v3_analog.upt`. Nothing on the host to install.

Reached. `STOCK=… ./sonix-packer/v3_analog_packer.sh` is the whole recipe: it
builds the player in `sonix-player/docker`'s image, builds the firmware-tools
image when it is missing, packs and verifies.

## Decision: two images, toolchain in the tree

The player's image carries the native build tools; `rockboxdev.sh` builds the
MIPS toolchain into `sonix-player/rockbox-toolchain`, FreeType and the audio
libraries next to it, all gitignored. The alternative — one image with the
toolchain baked into `/opt` — was declined: it makes the image 1-2 GB, takes
`docker build` out of the loop for `make`, and buys nothing a clone does not
already get, because the artifacts in the tree are what make the second run
fast.

## Done

- [x] `sonix-player/docker/{Dockerfile,build.sh}` — the player builds in a
      container, tree mounted at its own absolute path, container running as the
      calling user; `docker build` runs on every invocation, so a `Dockerfile`
      edit cannot be left out of the image
- [x] packer builds the player itself (`SKIP_PLAYER_BUILD=1` opts out), builds
      `tempotec-fw-tools` when missing, `cp -p` so `build_version` is the
      build's own time, refuses a `STOCK` inside the `build/` it clears
- [x] `verify` builds nothing and needs neither the stock image nor the player
- [x] `rockboxdev.sh --without-bluetooth-libs`: steps 7-11 (the Bluetooth
      userland libraries this port does not link) are skipped by decision, so
      the script finishes, cleans `tmp/rbdev-build`, keeps `tmp/rbdev-dl`
- [x] `rockboxdev.sh` wipes `$builddir` before a build: an interrupted run used
      to leave extracted-and-patched sources behind, and the patch loop then
      failed with rejects that named the wrong cause
- [x] `check-toolchain` (Makefile) and `build.sh` catch a half-built toolchain —
      a gcc with no libc in its sysroot, which `make` would otherwise call done
- [x] `ROCKBOX_TOOLCHAIN_DIR` from the environment reaches the build: `?=` in the
      Makefile and `RUN_EXTRA` actually passed to `docker run` (it was collected
      and dropped, so an external toolchain was silently ignored)
- [x] `warn()` writes to stderr: `$(os_version)` is captured, and a warning on
      stdout corrupted the version written into `system-info.json`
- [x] `sonix-player/tmp/` gitignored (2+ GB of rockboxdev workspace)
- [x] docs: `BUILD.md` (docker only, host build as an escape hatch),
      `V3-ANALOG.md`, `README.md`, `AGENTS.md`; `assets/BLAZE` described for what
      it is — the V3 Blaze port's assets, this port's base, not this device's own

## Evidence

- toolchain from scratch in the image, empty prefix: `exit=0`, sysroot has
  `libc.so.6` and `libasound.so`, Bluetooth steps reported skipped
- clean copy of the tree, no artifacts: player built in the container, `=== ok`,
  rootfs md5 `31459281486d8c089ca5d2fc1c4f526d`, kernel md5 stock
- second run in the same tree: 14 s, no `ROCKBOXDEV` line
- this tree: `=== ok`, rootfs md5 `ab5aa0c009989ee00f2cf852c56e50b3`

## Left to its owner, not blocking

- flash a build to the card and check it on the device
- the downloads (toolchain tarballs, FreeType, ogg/opus/wavpack, LVGL) carry no
  checksums; a truncated one fails later and less clearly
- no cache outside the tree: losing the tree costs the 20-60 minute toolchain
  build again, and the `tmp/rbdev-dl` tarball cache with it

## Rules this repository keeps

- do not patch `rockboxdev.sh` to make an optional step work — make the step
  optional
- do not build the toolchain on the host
- do not add a second wrapper script around the packer
- do not install anything on the host to get a build: a missing package is a
  `Dockerfile` edit
