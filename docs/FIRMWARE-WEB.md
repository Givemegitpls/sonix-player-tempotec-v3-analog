# The firmware page

A static page that turns the vendor's `v3_analog.upt` into this port's firmware
image **in the user's browser**. Nothing is uploaded; the page has no server side
at all.

It exists because of the licence, not for convenience: the built image is a
modified copy of the vendor's firmware — HiBy's binaries, Ingenic's kernel, the
vendor bootloader — and the vendor's licence does not allow publishing it
(`BUILD.md`, "Distributing an image"). Handing out a page that builds it on the
user's machine, out of the user's own copy, is the one way to ship the feature
without shipping the image.

```
web/index.html          the drop zone, the steps, the download link
web/app.js              UI, and the worker's message plumbing
web/worker.js           the build, off the main thread
web/lib/iso9660.js      reads the six files of the .upt, writes the new ISO
web/lib/ubifs.js        reads the rootfs out of SYSTEM.UBI
web/lib/vfs.js          the tree in memory, contents unpacked on demand
web/lib/install.js      install_tree.sh + trim_stock.py + fix_update_md5.py
web/lib/mkfs-ubifs.js   mounts the tree into wasm, calls mkfs.ubifs, reads it back
web/lib/payload.js      the port's own files, from a payload archive
web/lib/versions.js     the versions the selector offers (versions.json)
web/lib/tar.js          the tar layer (gzip is the browser's DecompressionStream)
web/lib/md5.js          UPDATE.TXT's digests; WebCrypto has no MD5
```

## What it does, step by step

1. Reads the `.upt` as ISO9660 and refuses anything whose `UBOOT.BIN` is not the
   V3 Analog's (`1f54858d6b4e6f482a81a3378c0e5bf8`). That digest is the one check
   that matters: a wrong bootloader is the only mistake this device cannot undo.
2. Unpacks the rootfs from `SYSTEM.UBI`. It is a **bare UBIFS image**, not a UBI
   container — no EC/VID headers — so the reader is a linear pass over the LEBs,
   keeping the highest `sqnum` per inode, per (inode, block) and per
   (parent, name). LZO data nodes are decompressed by the same wasm module that
   wrote them.
3. Installs the player, the launcher and the asset trees, rewrites the init
   script, sets `system-info.json`'s `OS_version` and `build_version`, drops the
   stock interface resources — `install_tree.sh`, ported.
4. Drops the stock player's decoders and the duplicated `libcrypto` — 7.45 MB —
   and proves nothing left refers to them (no remaining file names a removed
   soname, no dangling symlink under `lib/` or `usr/lib/`) — `trim_stock.py`,
   ported.
5. Packs the tree with `mkfs.ubifs` (**the real mtd-utils, compiled to wasm32**),
   same geometry as the packer: `-x lzo -e 0x1F000 -c 480 -m 0x800`.
6. Recomputes the MD5s in `UPDATE.TXT` (`fix_update_md5.py`) and writes the ISO.

The kernel, the bootloader, `VERSION.TXT` and `_GITIGNO` are carried through
byte for byte.

## Versions

The page offers **one version: the newest tag that carries a payload**. Not the tip
of a branch, and not the newest commit — a build that was never released cannot be
flashed with any confidence, so it is not offered.

`_site/versions.json` is generated at deploy time by
`sonix-packer/tools/make_versions_json.sh`, which:

1. lists the releases through the GitHub API and keeps those with a
   `firmware-payload-<board>.tar.gz` asset (attached by
   `.github/workflows/player-package.yml` when a tag is pushed);
2. takes the **greatest tag version** (`sort -V`), not the most recently published
   one: republishing an old tag does not make it the version to build;
3. takes that release's payload apart into the two halves the browser can fetch.

The label is the image's version, `<vendor firmware>/<port version>` —
`1.3.0/0.5.0` — because the tag alone would hide which vendor image the payload was
built on. `0.5.0` is the port's own tag; `1.3.0` is the vendor base marker and
carries no release.

The deploy does **not** build the player. It no longer needs the MIPS toolchain:
nothing here is built from the working tree, only the wasm `mkfs.ubifs` is, and
that is cached. A deploy is a couple of minutes rather than the hour a player build
costs.

While no release carries a payload, `versions.json` is written with an empty list
and the page says so — it does not offer a build it does not have.

A release is where a complete payload lives; the site does not keep a copy of it.
It cannot keep one either: a browser cannot fetch a GitHub release asset — the
download 302s to a signed object URL (`release-assets.githubusercontent.com`) that
answers without `Access-Control-Allow-Origin`, and `fetch()` from the page is
refused.

So the payload is served in two halves (`sonix-packer/tools/make_site_payload.sh`):

| served | what | size |
|---|---|---|
| `payload/<tag>.player.tar.gz` | `payload.json`, `sonix_player`, `sonix_launch` | ~2.9 MB |
| `payload/blobs/assets-<sha:12>.tar.gz` | `assets/BLAZE` + `assets/V3ANALOG` | ~22 MB, named by its content |

The blob name carries its digest, so the asset tree is stored once however many
versions use it, and the browser caches it. The whole site is ~26 MB. Every half
carries its sha256 in `versions.json`, and both are checked before anything is
unpacked (`sha256 mismatch` is a refusal, not a warning).

`stage_site.sh` writes a single `local` entry instead (`--local`), built from the
tree, because a local stage is for working on the page — that is the one place a
non-released build appears, and it never reaches the published site.

## What the page fetches, and why it is not the vendor's

The stock image has HiBy's player in it; the port's files are not derivable from
it. So the page downloads **one archive** from the same origin:

```
versions.json                        one entry: the newest released tag
payload/<tag>.player.tar.gz          payload.json, sonix_player, sonix_launch
payload/blobs/assets-<sha:12>.tar.gz  assets/BLAZE + assets/V3ANALOG
wasm/mkfs_ubifs.js                   the emscripten loader
wasm/mkfs_ubifs.wasm                 mkfs.ubifs + LZO + zlib, 152 KB
```

The complete payload (`firmware-payload-<board>.tar.gz`, ~26 MB) is what a release
publishes; the page never sees it whole.

`assets/BLAZE` and `assets/V3ANALOG` are what `v3_analog_packer.sh` installs over
the stock rootfs; both are in this repository and are published from it already.
The archive is ~26 MB (40.8 MB unpacked) and is cached by the browser the way any
static file is. `payload.json` inside it carries the version string and the build
stamp, which become `OS_version` and `build_version` in the image — the stamp has
to move between builds or the player will not offer the image as an update.

## Where the wasm comes from

`sonix-packer/tools/wasm/build.sh` builds it, in the image
`sonix-packer/docker/wasm` describes (Ubuntu 24.04 + emscripten 3.1.6). Sources
are pinned by sha256 in `tools/wasm/inside.sh`: mtd-utils 2.2.0 (the same version
the packer's tools image carries), lzo 2.10, zlib 1.3.2.

```sh
sonix-packer/tools/wasm/build.sh          # -> sonix-packer/build-wasm/out/
```

Three things in there are not obvious, and are commented where they live: Debian's
clang renames a wasm `main` to `__main_argc_argv` (so `mkfs.ubifs` is built with
`-Dmain=mkfs_ubifs_main`), emscripten's ioctl syscall aborts on an unknown request
(so `mkfs.ubifs` is built with `-Dioctl=wasm_ioctl_stub`), and the tree is mounted
through a custom read-only filesystem rather than copied into MEMFS (69 MB of
files, and modes come out of MEMFS defaults).

## Trying it locally

```sh
sonix-player/docker/build.sh target BOARD=tempotec_v3 -j"$(nproc)"   # once, 20-60 min
cp -p sonix-player/sonix_player sonix-player/sonix_launch sonix-packer/
sonix-packer/tools/wasm/build.sh
sonix-packer/tools/stage_site.sh
cd _site && python3 -m http.server 8000      # open http://localhost:8000/
```

`stage_site.sh` stages exactly what `.github/workflows/pages.yml` deploys; it calls
`make_versions_json.sh` without a repository, so a local stage offers its own build
alone. To see the release entries locally, run that script by hand with a
repository and a `GH_TOKEN` in the environment.

Publishing: the workflow deploys on every push to `main` that touches `web/`, the
packer's assets or the player. **Pages has to be enabled once** for the repository
(Settings → Pages → Source: GitHub Actions) — that is the owner's click, not
something a workflow can do for itself.

## Verifying a build without a device

`sonix-packer/tools/webtest/run.mjs` runs the page's own modules under Node on a
real image:

```sh
# the readers against ubi_reader's extraction of the same file (contents hashed)
node sonix-packer/tools/webtest/run.mjs read --ref /tmp/ref-stock

# the whole page: read, install, trim, pack, UPDATE.TXT -- compared with the tree
# sonix-packer/v3_analog_packer.sh built from the same binaries
SKIP_PLAYER_BUILD=1 sonix-packer/v3_analog_packer.sh              # the reference
node sonix-packer/tools/webtest/run.mjs build \
     --payload-archive /tmp/opencode/payload.tar.gz \
     --ref sonix-packer/build/verify/SYSTEM --out /tmp/ours.upt
```

Both pass on the stock 1.3 image today: 2545 entries read back byte for byte, and
a built image whose tree is identical to the packer's (916 entries, contents and
modes compared).

The reference tree is `sonix-packer/build/verify/SYSTEM`, which has ~916 entries —
that is the port's tree, not a truncated one: the stock interface's resources
(`usr/resource/litegui`, `layout`, `str`, `fonts`) are the bulk of the vendor
image's file count and `install_tree.sh` deletes them.

## What is not in the image, and what to expect

- **Podcast Index and Last.fm keys.** The packer puts a sealed
  `streaming-keys.bin` into the rootfs when it is present; it is not in the
  repository and the page does not ask for it. Those two services stay unsigned-in
  until an image is built with the keys (`BUILD.md` §7).
- **A different kernel.** `KERNEL_IMAGE=` is a packer option; the page always
  carries the stock `UIMAGE.BIN`. Nothing supported needs another kernel
  (`KERNEL.md`).
- **Addons.** They live on the card, not in the image (`ADDONS.md` in the addons
  repository).
- **Byte-identical rebuilds.** The tree is identical; the image is not byte for
  byte, for the same reason the packer's own builds are not: `mkfs.ubifs` writes a
  random filesystem UUID and files carry build-time mtimes.
- **Very old browsers.** The page needs ES modules in a worker,
  `DecompressionStream`, WebCrypto, and ~1 GB of memory while it runs (the tree
  and the image both live in the worker).
