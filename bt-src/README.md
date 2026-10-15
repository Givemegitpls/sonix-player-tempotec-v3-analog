# bt-src — bluealsa for the TempoTec V3 Analog

Cross-build of **bluez-alsa 4.0.0** for the TempoTec V3 Analog (Ingenic X1000E,
MIPS32r2, glibc 2.16), with the `--io-rt-priority=<n>` option **backported from
bluez-alsa 4.2.0**. The result is installed into the port's firmware assets as
`/usr/bin/bluealsa` and `/usr/lib/alsa-lib/libasound_module_pcm_bluealsa.so`.

The reason for the option: the player already probes for it and passes
`[bluetooth] io_rt_priority` (default 12) —
`sonix-player/src/system/bluetooth/bluetooth.c` calls
`bluealsa_knows_option("--io-rt-priority")`. Without it the player logs
`this bluealsa has no --io-rt-priority; the encoder stays at normal priority`,
so the A2DP encoder I/O thread cannot run at real-time priority.

## Result

| artefact | md5 | size |
|---|---|---|
| `out/bluealsa` | `883d9840d9dee2f0cce296c920869776` | 286 784 B |
| `out/libasound_module_pcm_bluealsa.so` | `4f476acc4136d8c31884129f4025000a` | 53 124 B |

Installed into (both layers, with a `.prev` of what was there):

- `sonix-packer/assets/BLAZE/usr/bin/bluealsa` (+ `.prev`)
- `sonix-packer/assets/V3ANALOG/usr/bin/bluealsa` (+ `.prev`)
- `sonix-packer/assets/{BLAZE,V3ANALOG}/usr/lib/alsa-lib/libasound_module_pcm_bluealsa.so` (+ `.prev`)

The effective image layer is `V3ANALOG` (the packer copies `BLAZE` first and
`V3ANALOG` over it, and its verifier compares the tree against
`assets/V3ANALOG/usr/bin/bluealsa`); `BLAZE` is updated too because the task
asked for it and it is the shared base of the port.

Firmware image: `sonix-packer/out/v3_analog.upt`,
**58 175 488 B, md5 `721a279c2031c432c2117fb4647a2120`**, player build stamp
**`151020262247`**. The previous image is kept as
`out/v3_analog-rc1-151020262247.upt` (md5 `7c4d79a6c8381799df33e23097461099`).

## Why 4.0.0 and not 4.2.0 / 4.3.1

`configure.ac` of 4.2.0 requires `glib-2.0 >= 2.58.2`; the device has
**glib 2.46.1**. Verified in the sources here:

```
bluez-alsa-4.0.0/configure.ac:  PKG_CHECK_MODULES([GLIB2], [glib-2.0 >= 2.32])
bluez-alsa-4.2.0/configure.ac:  PKG_CHECK_MODULES([GLIB2], [glib-2.0 >= 2.58.2])
```

4.0.0 is the newest release that builds against the device's glib.

## The ported feature: `--io-rt-priority`

In 4.2.0 the code lives in `src/main.c` (option parsing),
`src/ba-config.{c,h}` (the field) and `src/ba-transport-pcm.c` (the thread).
4.0.0 has a different file layout, so the same logic was ported to the
equivalent places — same option name, same semantics:

- `src/bluealsa-config.h` — `int io_thread_rt_priority;` in `struct ba_config`.
- `src/bluealsa-config.c` — `.io_thread_rt_priority = 0` (0 = off, the default).
- `src/main.c` — long option `{"io-rt-priority", required_argument, NULL, 3}`,
  the help line, and a case that validates the value with
  `sched_get_priority_min/max(SCHED_FIFO)` and exits on an out-of-range value
  (exactly as 4.2.0 does).
- `src/ba-transport.c` — in `ba_transport_thread_create()`, after
  `pthread_create()`, if `config.io_thread_rt_priority != 0` the thread is put
  on `SCHED_FIFO` at that priority with `pthread_setschedparam()`. **Failure to
  set the priority is logged and ignored** (`warn("Couldn't set IO thread RT
  priority: %s", ...)` and `ret = 0`), never fatal — the 4.2.0 behaviour.

The whole change is `patch/0001-io-rt-priority.patch` (generated with
`diff -ruN bluez-alsa-4.0.0 bluez-alsa-4.0.0.patched`; apply with `patch -p1`
from inside a pristine 4.0.0 tree). Nothing else in the upstream source is
touched.

## Directory layout

```
build.sh                 the whole build, from the tarballs to ./out
patch/0001-io-rt-priority.patch
pc/                      hand-written .pc files (bluez, the three ldacBT, the pcre/selinux/mount stubs)
src/bluez-alsa-4.0.0/            pristine 4.0.0 (used to build)
src/bluez-alsa-4.0.0.patched/    the same tree with the patch (used to regenerate the patch)
src/bluez-alsa-4.2.0/            reference only, for the ported feature
src/*.tar.gz                     the two upstream snapshots
third-party/                     bluez-5.66.tar.xz, sbc-2.1.tar.xz
debs/                            Debian mipsel dev/runtime packages (headers, .pc, runtime libs)
libs/                            the .so files the linker must see (from the device / stock tree)
sysroot/                         generated pseudo-sysroot (headers + .pc + .so)
build/                           generated work trees (sbc, patched bluez-alsa)
out/                             generated artefacts: bluealsa + the ALSA plugin
verify/                          the verification transcripts (see below)
```

Per the project's `AGENTS.md`, only the durable parts are meant for git
(`build.sh`, `README.md`, `patch/`, `pc/`, `verify/`); the extracted trees,
sysroots, debs, tarballs, `libs/`, `build/` and `out/` are on disk but
`.gitignore`d — `build.sh` recreates all of them.

## Where every source came from

All downloaded directly (no proxy was needed for these hosts).

| file | URL |
|---|---|
| `src/bluez-alsa-4.0.0.tar.gz` | `https://codeload.github.com/Arkq/bluez-alsa/tar.gz/refs/tags/v4.0.0` |
| `src/bluez-alsa-4.2.0.tar.gz` | `https://codeload.github.com/Arkq/bluez-alsa/tar.gz/refs/tags/v4.2.0` |
| `third-party/bluez-5.66.tar.xz` | `https://www.kernel.org/pub/linux/bluetooth/bluez-5.66.tar.xz` |
| `third-party/sbc-2.1.tar.xz` | `https://www.kernel.org/pub/linux/bluetooth/sbc-2.1.tar.xz` |
| `debs/libglib2.0-dev_2.42.1-1+b1_mipsel.deb` | `https://archive.debian.org/debian/pool/main/g/glib2.0/…` |
| `debs/libdbus-1-dev_1.10.32-0+deb9u1_mipsel.deb` | `https://archive.debian.org/debian/pool/main/d/dbus/…` |
| `debs/libfdk-aac-dev_2.0.1-1_mipsel.deb` | `https://archive.debian.org/debian/pool/non-free/f/fdk-aac/…` |
| `debs/libfdk-aac2_2.0.1-1_mipsel.deb` | same pool (runtime; not needed to link, kept for reference) |
| `debs/libopenaptx-dev_0.2.0-5_mipsel.deb` | `https://archive.debian.org/debian/pool/main/libo/libopenaptx/…` |
| `debs/libopenaptx0_0.2.0-5_mipsel.deb` | same pool (runtime) |
| `debs/libldacbt-enc-dev_…_mipsel.deb` | `https://archive.debian.org/debian/pool/main/libl/libldac/…` (provides `ldacBT.h`) |
| `debs/libldacbt-abr-dev_…_mipsel.deb` | same pool (provides `ldacBT_abr.h`) |

The BlueZ *headers* come from the bluez-5.66 tarball (`lib/*.h`); `bluez.pc` is
hand-written. `sbc` is built here (BlueZ ≥ 5.5 moved SBC out of its tree).

### The libraries the linker sees (`libs/`)

The device's own libraries are what the binaries load, so they are what the
linker must see (the same trick the project uses for glibc):

| file | origin |
|---|---|
| `libglib-2.0.so.0.4600.1`, `libgio-2.0.so.0.4600.1`, `libgobject-2.0.so.0.4600.1`, `libgmodule-2.0.so.0.4600.1`, `libdbus-1.so.3.14.4`, `libz.so.1.2.8`, `libffi.so.6.0.2` | pulled from the device `/usr/lib` (glib 2.46.1, dbus 1.10) |
| `libbluetooth.so.3` | pulled from the device (the project's own BlueZ 5.66 build) |
| `libfdk-aac.so.2`, `libopenaptx.so.0` | pulled from the device; identical code to the Debian runtime debs (only the ELF build-id differs) |
| `libldacBT.so` | the stock tree `tempotec v3/fw/SYSTEM/usr/lib/libldacBT.so` — the **combined** encoder+ABR+decoder library |

`libasound.so` (1.0.26) is copied from the rockbox toolchain sysroot, which
matches the device.

### The LDAC detail

The device's `libldacBT.so` carries the encoder, the ABR and the decoder in one
library. All three `.pc` files (`ldacBT-enc`, `ldacBT-abr`, `ldacBT-dec`) are
therefore **hand-written in `pc/` and point at `-lldacBT`** — mixing the
AOSP `libldacBT_enc` with the stock decoder puts two different LDAC handle
layouts in one process and segfaults (documented in the project's NOTES).

The project already keeps the AOSP-derived LDAC inputs under
`sonix-blaze/tools/ldac/`; those `.pc` files point at `-lldacBT_enc` /
`-lldacBT_abr`. The ones here differ on purpose: the shipped device binary's
`NEEDED` list contains **only `libldacBT.so`**, so this build links all three
against the combined library and reproduces that exactly (verified below).

The Debian `ldacBT.h` carries the encoder only (AOSP removed the decoder
upstream), so `build.sh` appends the two decoder prototypes bluez-alsa 4.0.0
calls: `ldacBT_init_handle_decode()` and `ldacBT_decode()`. They exist in the
stock combined library.

## Exact build steps

`build.sh` does all of it. The lines that matter:

**sbc** (static, because the device has no `libsbc.so`):

```
./configure --host=mipsel-rockbox-linux-gnu --prefix=/usr \
    --disable-shared --enable-static --disable-tester --disable-tools \
    CC=…-gcc AR=…-ar RANLIB=…-ranlib CFLAGS="-O2 -march=mips32r2"
make -j$(nproc) && make install DESTDIR=$sysroot
```

**bluealsa** (the GitHub snapshot has no `configure`, so `autoreconf -fi` first):

```
export PKG_CONFIG_SYSROOT_DIR=$sysroot
export PKG_CONFIG_LIBDIR=$sysroot/usr/lib/pkgconfig
autoreconf -fi
./configure --host=mipsel-rockbox-linux-gnu --prefix=/usr \
    CC=…-gcc AR=…-ar RANLIB=…-ranlib STRIP=…-strip \
    CFLAGS="-O2 -march=mips32r2" LDFLAGS="-Wl,--allow-shlib-undefined" \
    --enable-aac --enable-aptx --enable-aptx-hd --enable-ldac --with-libopenaptx \
    --disable-aplay --disable-systemd --disable-upower
make -j$(nproc) LIBS="-lrt -lz -lffi -lm -lgmodule-2.0"
```

`--with-libopenaptx` matters: bluez-alsa 4.0.0 otherwise wants the proprietary
Qualcomm apt-X API, while the device has `libopenaptx`. `--allow-shlib-undefined`
is needed because the stock `libldacBT.so` has undefined `ptn0`/`ptn3`/`AS`
symbols that nothing defines (they are not called).

**the ALSA plugin** is then relinked with the project's glibc-2.16 shim
(`tools/glibc216_shim.c`), which defines `clock_gettime` locally so the
versioned `GLIBC_2.17` reference leaves the binary:

```
…-gcc -O2 -march=mips32r2 -fPIC -c tools/glibc216_shim.c -o glibc216_shim.o
…-gcc -shared -Wl,-soname,libasound_module_pcm_bluealsa.so \
    -o libasound_module_pcm_bluealsa.so \
    src/asound/.libs/bluealsa-pcm.o src/shared/.libs/{a2dp-codecs,dbus-client,hex,log,rt}.o \
    glibc216_shim.o -L$sysroot/usr/lib -lasound -ldbus-1 \
    -lrt -lz -lffi -lm -lgmodule-2.0 -Wl,--allow-shlib-undefined
```

## Rebuild from scratch

```
cd src/sonix-blaze/bt-src
./build.sh          # sysroot -> sbc -> bluealsa -> ./out
```

`build.sh` needs `debs/`, `third-party/` and `libs/` (all committed here), the
rockbox toolchain at `../../toolchain/rockbox-toolchain`, and the project's
`tools/glibc216_shim.c`. It does **not** need the device or the network.

To regenerate the patch: `diff -ruN src/bluez-alsa-4.0.0 src/bluez-alsa-4.0.0.patched > patch/0001-io-rt-priority.patch`.

To install into the assets:

```
cp out/bluealsa                     ../sonix-packer/assets/V3ANALOG/usr/bin/bluealsa
cp out/libasound_module_pcm_bluealsa.so \
   ../sonix-packer/assets/V3ANALOG/usr/lib/alsa-lib/libasound_module_pcm_bluealsa.so
# (and the same under assets/BLAZE/)
```

To rebuild the firmware image:

```
cd ../sonix-packer
STOCK=/home/alex/Projects/own/tempotec-v3/firmware/tt/fw.upt ./v3_analog_packer.sh
```

## Verification

Transcripts are in `verify/`.

**1. `--help` lists the option, and the binary runs on the device.** Copied to
`/tmp/bluealsa-new` on the device (never over `/usr/bin/bluealsa`):

```
$ /tmp/bluealsa-new --help
  …
  --io-rt-priority=NUM          real-time priority for IO threads
  …
Available BT audio codecs:
  a2dp-source:  SBC, AAC, aptX, aptX-HD, LDAC
  a2dp-sink:    SBC, AAC, aptX, aptX-HD, LDAC
  hfp-*:        CVSD
```

The priority validation matches 4.2.0:

```
$ /tmp/bluealsa-new -p a2dp-source --io-rt-priority=100
/tmp/bluealsa-new: E: Invalid IO thread RT priority [1, 99]: 100   (exit 1)
$ /tmp/bluealsa-new -p a2dp-source --io-rt-priority=0
/tmp/bluealsa-new: E: Invalid IO thread RT priority [1, 99]: 0     (exit 1)
```

**2. `readelf -d` NEEDED list is identical to the shipped binary**
(`verify/elf.txt`; the shipped one was pulled from the device to
`/tmp/opencode/bluealsa.device`). Both:

```
libfdk-aac.so.2  libopenaptx.so.0  libbluetooth.so.3  libgio-2.0.so.0
libgobject-2.0.so.0  libglib-2.0.so.0  libldacBT.so  librt.so.1  libz.so.1
libffi.so.6  libm.so.6  libgmodule-2.0.so.0  libpthread.so.0  libc.so.6
```

**3. No glibc symbol above 2.16.** `readelf -V` max is `GLIBC_2.12` for
`bluealsa` and `GLIBC_2.7` for the plugin.

## Caveats

- `build/` and `sysroot/` are generated; they are kept here so the build can be
  inspected, but `build.sh` regenerates both.
- `assets/BLAZE/usr/bin/bluealsa` was the Blaze port's original binary
  (`9500aec9…`, needs GLIBC 2.17); it is kept as `bluealsa.prev`. The V3
  Analog's previous binary (`369b2dbb…`) is kept as `V3ANALOG/usr/bin/bluealsa.prev`.
- The device was only read from: files were pulled and a copy of the new binary
  was run from `/tmp`. `/usr/bin/bluealsa` on the device was **not** replaced,
  and nothing was flashed.
