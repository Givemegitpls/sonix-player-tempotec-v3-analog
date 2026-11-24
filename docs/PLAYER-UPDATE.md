# Updating the player over the air

Status: **the path is complete in the tree.** The package, the CI, the player's
menu, download, verification and staging, and the launcher's swap all exist —
"What exists" lists them. What has been run on the device is the package, the CI
and the launcher's half; the player's own download-and-stage flow is the part
that has not been exercised there yet.

Implemented: the whole network path -- index, download, verification, staging --
and its card in Settings → System → Update firmware → "Via internet", plus
`make package` and the CI that builds it per device
(`.github/workflows/player-package.yml`, section "What exists"), the version and
the board id compiled into the binary, the `--verify` mode and the healthy mark
in the player, and the launcher's state machine below.

## What is updated, and what is not

An update replaces **the player binary**, nothing else. Not the kernel, not the
rootfs, not the vendor's bootloader, and not `sonix_launch`.

There are two reasons and they point the same way:

- the built `.upt` is a modified copy of the vendor's firmware -- HiBy's
  binaries, Ingenic's kernel, the vendor bootloader -- and publishing it is not
  ours to do (`docs/BUILD.md`, "Distributing an image"). A package holding only
  the player is GPL-3.0 code of this repository and can be handed out;
- the launcher is the way back from a player that does not start. An update that
  can break the way back has no way back.

The image path is untouched: a `.upt` on the card, flashed through recovery, is
still the full update and the last resort. Nothing below changes it.

## The package

```
sonix-player-update-<board-id>.tar
  manifest.json
  sonix_player
```

A plain tar, not a `.tar.gz`, and that is a deliberate trade: the player runs
with about a megabyte of free RAM (LVGL holds the rest), and inflating needs a
32 KB dictionary plus a streaming state machine inside the one program that must
not run out of memory. 6.7 MB over Wi-Fi instead of 2.9 is the cheaper side of
it; if compression comes back, it comes back with a streaming inflate that is
tested on its own first.

```json
{
  "kind": "player",
  "format": 1,
  "board": "v3_analog",
  "version": "1.3.0/1.3.0",
  "requires_image": "1.3.0",
  "notes": "- The player can be updated over the air.\n- ...",
  "files": [
    { "name": "sonix_player", "size": 7008116, "mode": "0755", "sha256": "…" }
  ]
}
```

`notes` is the **message of the annotated tag** the version names, escaped by
`tools/make_update_package.sh` into the manifest and into the `<board>.json`
fragment, so `index.json` carries it too. A release is therefore one commit --
bump `"version"`, write the tag message as the changelog, tag -- and the update
card shows that text through the same `render_notes()` the firmware card has
always used. `NOTES_FILE` overrides it for a test build with no tag.

`board` is board.h's update stem, not the human name: it is the stable,
filesystem-safe identifier of a device, and a package naming another board is
refused. `version` is the string compiled into the binary -- `<base>/<tag>`,
`<base>/#<commit>` off a tag, `<base>/<md5>` for a tree that is not a commit
(`os_version()` in `sonix-packer/v3_analog_packer.sh`), with `-dirty` on top
when the tree carries changes.

The archive is reproducible: fixed mtimes, owners and order, so two builds of one
commit differ only in the compression's own bytes.

## What exists

- **The package.** `make package` (the Makefile section "The over-the-air
  package") calls `tools/make_update_package.sh`, which writes
  `sonix-player-update-<board-id>.tar`, its `.sha256` and a `<board-id>.json`
  fragment for `index.json`. The CI builds it per board, gates it on the glibc
  2.16 ABI, on a stale addon exports list and on a size the installer can take,
  and on a tag publishes one release with every board's package and the index.
- **The version and the board** are compiled in (`version.h`), reported as the
  first line of the log, and used by `--version-check` and by the package check.
- **The player's half**: `--install-package [path]` verifies a package on the card
  and stages it; `--install-update <url>` downloads it first (streamed to the
  card); `--verify` is what the launcher calls; `--version-check` answers the
  version rule. All of them run before the display exists, so they work over adb
  beside a running player.
- **The launcher's half** (`launcher/sonix_launch.c`): the lock, the backup on the
  card, the swap with the player stopped, the rollback when the lock survives.
- **The one way in**: Settings → System → Update firmware → "Via internet". Nothing
  at start-up, no menu row of its own -- the player looks at the network when the
  user asks and at no other time.

## The installer

```
/usr/bin/sonix_player            the player (home)
<card>/sonix/player.new          the package's binary, staged
<card>/sonix/player.sha256       its digest
<card>/sonix/player.prev         the copy of what was running before
<card>/sonix/pending             {version} -- "there is something to install"
/data/update.lock                {new_sha256} -- "a swap is in progress"
```

One marker, not two. The launcher writes the lock before it swaps the binary in;
the player deletes it once its interface is up. A lock that survives a boot is
therefore the one unambiguous statement "the build written for it never came up".
An earlier design had a second "healthy" file written by the player, and it cost
two rollbacks on the device: the leftover marker of one update was read as the
failure of the next.

1. **The player, asked from the menu.** Fetches the index, picks this board's
   entry, decides on the version, downloads the package to the card, holds the
   archive to the index's digest, verifies the manifest inside it (board, version,
   the binary's own size and sha256), writes `pending` and restarts through the
   power menu's own shutdown.
2. **The launcher, before it starts anything.** No pending: start the player, as
   always. Pending: verify the payload with `--verify`, copy the current binary to
   `player.prev` and read it back, write the lock, unlink the old binary, copy the
   new one in, `fsync`, `chmod 755`, `exec` it.
3. **The new player.** Interface up: delete the lock, then the card's staging
   files. That deletion is the confirmation -- there is no second marker.
4. **If it never comes up.** The lock is still there after the reboot, so the
   launcher restores `player.prev`, drops the lock and the staging files, and
   starts the build that worked.

The rootfs never holds two copies: at the swap the player is not running, so `rm`
really frees the old file's blocks, and the backup lives on the card. A power cut
during the copy leaves a half-written file and the lock; the next boot restores
the backup from the card.

The lock also records the **rootfs's own build id** (`build_version` out of
`system-info.json`, read by the launcher and written beside `new=`). A flash
rewrites the rootfs and leaves `/data` alone, so a lock carried through a flash
belongs to a rootfs that no longer exists -- the launcher recognises that from the
mismatch and drops the lock instead of restoring the backup over a freshly flashed
image. That exact mistake happened on the device: a stale lock from 0.5.5 rolled
the just-flashed player back to 0.5.2 on the first boot.

## Small changes over the air, big ones from the card

A package may name the rootfs it needs, `requires_image`, as a vendor base
(`"1.3.0"`). The player compares it with the image it is actually running
(`OS_version`'s base, read from `system-info.json`) and refuses the install when
the image is older -- the card says which image is needed. That is the mechanism
that sends a user to the card: a change touching `launcher/`, `sonix-packer/assets/`
or anything else outside the player cannot travel as a package, and the package
built afterwards carries the requirement.

The image itself still cannot be downloaded from the device: it holds the vendor's
kernel, bootloader and binaries, and publishing it is not ours to do
(`docs/BUILD.md`, section 8). So the device can refuse, explain and point at the
release page -- and the image is built locally or handed over by the owner. The
rule of thumb for what a release is: if the diff between two tags touches the
launcher or the packer's assets, it is an image release; everything under `src/`
is a package.

## Which version wins

The rule is `src/system/core/semver.c`, executable as a table in
`sonix-player/tools/semver_test.c` (both a laptop and the CI build it with the
host compiler -- no SDL, no MVS toolchain). Only the part after the last `/` is
compared; the base names the vendor firmware the rootfs came from.

| package | running | verdict | why |
|---|---|---|---|
| `…/0.4.1` | `…/0.4.0` | newer | tags compare numerically, component by component |
| `…/0.10.0` | `…/0.9.0` | newer | numeric, not lexical |
| `…/0.3.9` | `…/0.4.0` | older | refused, with the two numbers in the sentence |
| `…/1.3` | `…/1.3.0` | same | a missing component is zero |
| `…/1.3.0` | `…/#abc1234` | newer | a release over a commit |
| `…/1.3.0` | `…/0.4.0-dirty` | newer | a release over a dirty build |
| `…/#abc1234` | `…/#abc1234` | same | the same build |
| `…/#abc1234` | `…/1.3.0` | ask | commit hashes have no order: user decides |
| `…/0.4.1-dirty` | `…/0.4.0` | ask | a dirty build never installs by itself, whatever its number |
| `…/9f2c1a4b` | `…/1.3.0` | ask | a build outside git has no order either |

What the check does, in order: fetch `index.json` (one HTTP GET, ~300 bytes),
find the entry whose `board` is this device, compare its `version` with
`player_version()`, and only then fetch the package it names -- holding the
archive to the `sha256` the index publishes, and the binary inside it to the
`sha256` the manifest publishes. Checked on a host build against a local server:
`newer` downloads and stages, `same` prints "nothing to do", a wrong archive
digest refuses with both digests and leaves nothing staged.

"ask" is the same card with both versions named: the user pressing Install *is* the
confirmation, so nothing has to be remembered.

## Where the index comes from

`https://github.com/Givemegitpls/sonix-player-tempotec-v3-analog/releases/latest/download/index.json`
-- this repository's newest release, as its own asset path, so no API call and no
token is involved. The URL is compiled in (`PLAYERUPDATE_INDEX_URL`,
`src/system/device/playerupdate.c`): one board, one project, one place to look.
`[update] index_url` in `device_config.ini` overrides it, which is how a local
server is tested against without rebuilding.

What the device trusts, then, is exactly: the repository, over TLS, plus the
sha256 the index publishes for the archive. That is the trade recorded above --
no signature, so a compromised repository is a compromised update channel, and
the honest documentation of that is a feature of this design rather than an
oversight.

## Still to do

1. **Publish.** The hardcoded index URL points at a release asset that does not
   exist yet; until the CI job (or a manual `gh release upload`) puts
   `index.json` and the package into a release, the menu's check answers "check
   failed" on a real device. The local-server path is what the tests use.
2. A device run of the *menu* path from a flashed image with this state machine
   (the previous image still carried the two-marker design): Install from the card,
   then Install over the network, then "up to date", then a package that fails to
   start and the automatic rollback.
3. The image build now passes `BUILD_VERSION_OVERRIDE` from the packer's own
   `os_version()`, because the container sees only the player tree and `git
   describe` there cannot see the tags -- the first image called itself
   `1.3.0/unknown`.
4. A signature, if packages are ever published beyond the owner's own builds:
   ed25519 over the manifest, public key compiled in. Until then sha256 is what
   protects against corruption, not against a replaced release -- a trade taken
   deliberately with the repository as the anchor.
5. The card-image path (`.upt` through recovery) is untouched and remains the way
   back; it is worth one regression run after the state machine changed.
