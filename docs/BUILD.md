# Building the image

Everything here is done on a Linux host. The result is one file,
`sonix-packer/out/v3_analog.upt`: the TempoTec V3 Analog's own update image,
built on top of the vendor's OFW **1.3** with this player in place of the stock
one.

The stock firmware is **not** in the repository — it is the vendor's, and the
port only carries what it adds to it. You need one copy of it (below), and
nothing else that is not public.

---

## 0. What you need

A Linux host (x86_64) with **docker**, about 6 GB of free disk, and an internet
connection for the first build. Nothing else: both compilers the build uses live
in images this repository carries the recipes for, so there is no list of
packages to install.

```sh
docker build -t tempotec-player-build sonix-player/docker   # builds the player
docker build -t tempotec-fw-tools sonix-packer/docker       # packs the image
```

Both are built from pinned bases, so the same recipe produces the same tools
later, and neither has to be built by hand: `sonix-player/docker/build.sh` builds
its own image on every run (docker reuses the layers of an unchanged
`Dockerfile` in a second), and the packer builds its image on the first run
because it cannot run `7z`, `mkfs.ubifs`, `genisoimage` or `ubi_reader` without
it — none of which a normal host has.

What the images carry, in one line each: the player's image has the native
compiler, autotools, makeinfo and the archivers that `rockboxdev.sh` and the
library `configure` scripts need, because the MIPS toolchain and FreeType are
built from source; the packer's has the firmware tools.

If you would rather have those packages on the machine itself, the build runs on
the host too — see "Building on the host", at the end.

---

## 1. Get the tree

```sh
git clone https://github.com/Givemegitpls/sonix-player-tempotec-v3-analog
cd sonix-player-tempotec-v3-analog
```

---

## 2. Get the stock firmware (OFW 1.3)

The port is built **on top of** the vendor's `v3_analog.upt`, the build this
device shipped with and the one everything was measured against: `VERSION.TXT`
says `name=v3_analog`, `ver=2024-12-24T11:00:26+08:00`.

It is not in this repository and no mirror is hosted here. Take it from the
vendor's update, from the box's own card, or from a forum copy of the 1.3
firmware (the 4pda thread for the device has it). Whatever the source, the two
files that matter must be these:

| file inside the `.upt` | md5 |
|---|---|
| `UBOOT.BIN` | `1f54858d6b4e6f482a81a3378c0e5bf8` |
| `UIMAGE.BIN` | `35889275f8bed2f967a18f6a8b66495c` |

The packer checks `UBOOT.BIN` itself and refuses to build an image whose
bootloader is not the one above — a wrong bootloader is the one mistake on this
device that cannot be undone from a card. `UIMAGE.BIN` is compared by size and
digest when a kernel is installed instead (step 6).

To look before you build:

```sh
# 7z lives in the packer's image, so nothing has to be installed for this
docker run --rm -v "$PWD":/w -w /w tempotec-fw-tools 7z l v3_analog.upt   # the six files inside the ISO
docker run --rm -v "$PWD":/w -w /w tempotec-fw-tools 7z x v3_analog.upt -ostock-unpacked
md5sum stock-unpacked/UBOOT.BIN stock-unpacked/UIMAGE.BIN
```

Put the image where you like and pass its path to the packer with `STOCK=`
(see step 6).

---

## 3. Build the player

Nothing to do here either: step 5 builds the player in the image
`sonix-player/docker` describes and puts both binaries next to itself. On a
machine with nothing but docker that is the whole recipe; the step is in this
document because it is where the time goes.

It runs on every packer run, not only when the binaries are missing: `make`
answers "nothing to do" in a second when the sources have not changed, and a
binary left from an earlier build would otherwise be packed into an image that
claims to be this commit's. `SKIP_PLAYER_BUILD=1` packs the binaries that are
already in `sonix-packer/`, for a run that only wants to repack.

To build the player on its own, without packing:

```sh
sonix-player/docker/build.sh
```

That is `make target -j"$(nproc)"` in that image, with the tree mounted at its
own path and the container running as you — the outputs are ordinary files of
yours, in the tree. Anything `make` takes goes through: `build.sh host` builds
the SDL simulator, `build.sh target -j2` builds with two jobs.

The **first** run does four things by itself and takes a while (20–60 minutes
on a laptop), all of it inside the container:

1. builds the Rockbox MIPS cross toolchain into `sonix-player/rockbox-toolchain`
   (binutils, gcc 9.5, Linux headers, glibc 2.27 with `--enable-oldest-abi=2.16`
   so that the device's glibc 2.16 is satisfied);
2. downloads and cross-builds FreeType, static, into `freetype-target/`;
3. downloads and cross-builds libogg, libopus, opusfile and libwavpack, static,
   into `audio-target/`;
4. clones LVGL 9.6 and builds the player itself.

Later runs go straight to step 4 and take a couple of minutes: all four live in
the tree, not in the image, so they are built once.

The toolchain build stops after alsa-lib, on purpose: the steps after it
(libffi, zlib, glib, expat, dbus) are the libraries of a Bluetooth userland
built from source, and this port does not link them — its Bluetooth userland is
prebuilt, in `sonix-packer/assets`, and the player links `libc` and `libasound`
out of the sysroot and nothing else. `rockboxdev.sh --without-bluetooth-libs`
says so, finishes, cleans its workspace and exits 0; without the flag it tries
those steps and dies in glib's `configure`, which finds no gettext.

If a toolchain already exists — from another checkout of this port, or from an
earlier build — point the build at it instead of rebuilding: `TOOLCHAIN_DIR` is
not a thing, the Makefile's variable is `ROCKBOX_TOOLCHAIN_DIR`, and
`sonix-player/docker/build.sh` passes the environment through:

```sh
ROCKBOX_TOOLCHAIN_DIR=/path/to/rockbox-toolchain sonix-player/docker/build.sh
```

Two things can stall the downloads on some networks; both have a way around
them:

- FreeType comes from `download.savannah.gnu.org`. If that stalls, fetch
  `freetype-2.13.3.tar.xz` from SourceForge by hand and drop it in
  `sonix-player/` — the Makefile uses it instead of downloading.
- LVGL is cloned from GitHub. A half-finished clone leaves `sonix-player/lvgl/`
  behind; remove that directory before retrying.

The build ends with two files in `sonix-player/`:

```
sonix_player         the player
sonix_launch         the launcher that reboots the device if the player exits
```

and with a check that matters:

```
check-abi: no symbol beyond glibc 2.16.
```

If that line says something else, do not flash the result: the binary asks for
a glibc symbol the device does not have, and the device will boot-loop instead
of starting the player.

To build the SDL simulator instead (no toolchain, no device) install
`libsdl2-dev libfreetype-dev libopusfile-dev libwavpack-dev libasound2-dev`
(Debian names; `sdl2 freetype2 opusfile wavpack alsa-lib` on Arch), then:

```sh
make host
./sonix_player_host
```

This one is a host build on purpose: the image has no SDL, and the simulator is
only ever run on a desktop.

---

## 4. Where the binaries come from

`sonix-packer/sonix_player` and `sonix_launch` are the copies the packer installs
into the rootfs. It rebuilds and copies them on every run (step 3), so they are
never older than the sources: an unchanged tree costs a second of `make`, a
changed one costs the compile. `SKIP_PLAYER_BUILD=1` leaves them alone.

---

## 5. Build the image

```sh
cd sonix-packer
STOCK=/path/to/v3_analog.upt ./v3_analog_packer.sh
```

With no `STOCK=` it looks for `sonix-packer/v3_analog_original.upt`; drop the
vendor image there if you prefer, that name is git-ignored. Keep it out of
`sonix-packer/build/`: that directory is cleared before the stock image is
unpacked, and the script refuses a stock image it is about to delete.

First it builds the player (step 3) and copies `sonix_player` and
`sonix_launch` next to itself, which `SKIP_PLAYER_BUILD=1` skips. Then, in order:

1. unpacks the stock ISO (it *is* an ISO, despite the extension);
2. extracts the UBIFS rootfs from `SYSTEM.UBI`;
3. deletes `usr/bin/hiby_player` and installs `usr/bin/sonix_player`, the
   launcher and `assets/BLAZE` + `assets/V3ANALOG` over the tree, and points
   `etc/init.d/S92_03_start_music_player` at the new launcher;
4. repacks the rootfs with `mkfs.ubifs` (`-x lzo -e 0x1F000 -c 480 -m 0x800`,
   the geometry of this device's rootfs volume — it fails loudly if the tree no
   longer fits);
5. recomputes the digests in `UPDATE.TXT` (the updater checks them) and builds
   the ISO with `genisoimage`;
6. unpacks its own result and checks it: digests, files, modes, and that the
   Bluetooth userland in the image is the port's and not the stock one.

The result:

```
out/v3_analog.upt
```

with `UBOOT.BIN` carried through byte for byte and the stock `UIMAGE.BIN`
unless a kernel is installed on purpose.

Useful invocations:

```sh
STOCK=... DOCKER_IMAGE=tempotec-fw-tools ./v3_analog_packer.sh   # the defaults
./v3_analog_packer.sh verify                                     # re-check out/v3_analog.upt, build nothing
SKIP_PLAYER_BUILD=1 ./v3_analog_packer.sh                        # repack the binaries already in sonix-packer/
KERNEL_IMAGE=/path/UIMAGE.BIN ./v3_analog_packer.sh              # install a replacement kernel
VERSION_OVERRIDE=1.2.0.1/0.1.0 ./v3_analog_packer.sh             # name the build by hand
```

`OS_version` in `usr/resource/sonix/components/system-info.json` is the string
the player shows as **Operating system version**; the packer writes it per build
(a tag, a commit, or an md5 of uncommitted changes) and `build_version` is the
binary's own timestamp.

---

## 6. Flash it

1. Copy `out/v3_analog.upt` to the **root of the microSD card**, with exactly
   that name — the updater looks for `v3_analog.upt` and nothing else.
2. Charge the player above 30 % — the player's own update entry refuses below
   that, and it is right to: a flash that loses power halfway cannot be undone
   from the interface.
3. Start the update:
   - from the stock firmware: its own firmware-update entry, from the card;
   - from this player: **Settings → System → Update firmware → From SD card**;
   - or hold **Power + Prev** while switching on to enter recovery.
4. The device says "Upgrading…", then "Succeeded", and reboots.

`UBOOT.BIN` is never written by an update: the bootloader is the vendor's and
stays as it is, which is what keeps a failed flash recoverable from the card.

Going back is the same procedure with the vendor's own `v3_analog.upt`.

---

## 7. Streaming services

Qobuz, Tidal, Podcast Index and Last.fm need an account, and no application
credentials are compiled into the image. They go on the **card**:

```
<card>/sonix/streaming.json
```

```json
{
  "qobuz": {"token": "…"},
  "tidal": {"access_token": "…", "refresh_token": "…"}
}
```

What the player does on its own: it takes Qobuz's application id and secret
from Qobuz's web player when it first needs them (the same derivation the
open-source downloaders use), keeps them in `sonix/streaming-session.json` on
the card, and fetches them again if Qobuz changes them. Nothing of that is in
the image, so a rotation costs a fetch, not a new firmware.

Tidal's application `client_id`/`client_secret` are public and are built in;
they can be overridden in the same file (`"client_id"`, `"client_secret"`).

The Qobuz token is the one the web player keeps in local storage: sign in at
`play.qobuz.com`, then **F12 → Application → Local Storage → localuser →
token**.

---

## 8. Distributing an image

The built `.upt` is a modified copy of the vendor's firmware: it contains
HiBy's binaries (`sys_server`, the stock codecs), Ingenic's kernel, the vendor
bootloader. Building it for yourself is one thing; publishing it is another, and
the vendor's licence does not allow that. Hence: this repository carries the
sources and the scripts, not an image.

---

## Building on the host

The image supplies the tools; the recipe is the Makefile, and it runs on the
machine just as well. What it needs there, beyond a C/C++ compiler and `make`:

```sh
# Debian / Ubuntu
sudo apt install build-essential git wget xz-utils bzip2 texinfo \
                 autoconf automake libtool libtool-bin pkg-config p7zip-full
# Fedora
sudo dnf install gcc gcc-c++ make git wget xz bzip2 texinfo \
                 autoconf automake libtool libtool-bin pkgconf-pkg-config p7zip
```

`libtool-bin` is a package of its own on Debian and Ubuntu, and it is the one
that matters: `rockboxdev.sh` looks for the `libtool` script by name, and the
`libtool` package carries only the macros and `libtoolize`.

Then, from `sonix-player/`:

```sh
make target -j"$(nproc)"
```

The toolchain, FreeType and the audio libraries land in the tree exactly as they
do in the container, so the two can be mixed — a tree built once on the host can
be rebuilt in the image and the other way round. A toolchain that already exists
somewhere else is reused rather than rebuilt:

```sh
make ROCKBOX_TOOLCHAIN_DIR=/path/to/rockbox-toolchain target -j"$(nproc)"
```

This list is exactly what `sonix-player/docker/Dockerfile` installs, so a package
missing there is missing here too.

