# Taking a newer Sonix Player release

The player's source is a fork of
[Jepl4r/sonix-player](https://github.com/Jepl4r/sonix-player) and this tree
carries that project's history: an update is a **merge of its release tag**,
not a patch applied by hand. What follows is the recipe, in the order it has to
be done, with the places that hurt.

The merge this document was written from: base `d9a0f1b` (10 October 2026) to
tag `1.3.0` (`a488e25`, 16 October) — 67 upstream commits, 84 of our own,
**39 conflicts in 37 files**, ~100 hunks, and an afternoon of interface
scaling.

`AGENTS.md` has the rules; `docs/BUILD.md` has the build; this file has only
the update.

## What the merge has to keep

Upstream does not know about this board. These files decide what "this board"
means, and in a conflict they are ours:

| file | what it decides |
|---|---|
| `sonix-player/src/system/device/board.h` | one build, one board: panel, mixer card, PCM, backlight node, keys, USB gadget, panel sleep |
| `sonix-player/src/system/device/sysinfo.h` | the model struct `board.h` fills in: the fields every board-specific branch reads |
| `sonix-player/src/gui/shell/scale.h` | the interface compiles at two thirds (`SC()`) |
| `sonix-player/src/main.c` | the 32 bpp single-page SLCD path, `FBIOPAN_DISPLAY`, the damage-pan, the frame profiler |
| `sonix-player/src/system/device/power.c` | screen-off through the vendor `SET_SLEEP` ioctl, the wake log, no `FBIOBLANK` ever |
| `sonix-player/src/system/audio/alsa-controls.c` | the AK4493 mixer on card 1, the route parked before suspend |
| `sonix-player/src/system/device/bootlogo.c` | `bootlogo_board_supports()`: no boot-logo marker on a board whose `/dev/mtd5` is the `/data` volume |
| `sonix-player/src/gui/shell/icons_small.c` | the icon set this panel draws (generated, see below) |
| `sonix-packer/` | the V3 packer, its assets and its docker image — upstream's packer is a different program for different players |

The invariants that must survive any merge, because each of them is the
difference between a player and a brick or a boot loop:

- **no `FBIOBLANK`** on this panel: screen-off is the vendor `SET_SLEEP` ioctl
  (`panel_sleep_ioctl`), and the backlight alone comes first;
- **one framebuffer page** and the pan: a frame reaches the glass only through
  `FBIOPAN_DISPLAY`;
- the codec is **ALSA card 1** (`hw:1` for the mixer, `plughw:1,0` for playback);
- **nothing newer than glibc 2.16** may leave the binary — `make check-abi`
  decides, and it is run by the target build itself;
- **`/dev/mtd5` is the `/data` UBI volume** here, not a scratch partition;
- Bluetooth is our **BlueZ 5** userland from `assets/BLAZE`, not the stock 4.101;
- upstream's kernel and kernel modules are **not** used: TempoTec's 3.10 kernel
  stays. So `axp2101.ko`, `mp2731.ko`, `soc_fb.ko`, `soc_msc.ko`, `brcmfmac.ko`,
  zram and anything that needs them is out.

## 1. Fetch the tag and merge it on a branch

A clean tree matters: the packer names the build after the tag when HEAD is
clean, and after an md5 of the diff when it is not.

```sh
git remote add upstream https://github.com/Jepl4r/sonix-player.git   # once
git switch -c upstream-<version>
git fetch upstream tag <version>
git merge --no-ff <version>
```

## 2. Resolve the conflicts by group

Do not try to make each hunk agree with both sides. Decide what the file is for,
then take that side's structure:

- **display, power, audio** (`main.c`, `power.c`, `bootlogo.c`, `audio.c`,
  `alsa-controls.c`, `device_state.c`, `art_shrink.c`): our code stays, and
  upstream's new behaviour is ported **into** it — the refresh pause while the
  screen is dark (`refr_request_cb`), the battery check during standby
  (`rtc_alarm_arm`), the asynchronous route re-init on resume, metronome-aware
  idle detection. Our side is the measurements; theirs is the feature.
- **interface pages** (`gui/**`): upstream's structure wins, then the page is
  scaled (step 3). Where our side had a feature they do not — the frequency
  keypad in the PEQ, the deferred row binding in `medialist.c`, the caret
  parking in `keyboard.c` — ours is kept and their new plumbing is added around
  it.
- **whole-file reworks** (`cover.c`, `coverloader.c`, `podcastpage.c`,
  `scrolltext.c`): take upstream's file (`git checkout --theirs -- <file>`), then
  re-apply what the port needed. For 1.3.0 that was one hook
  (`coverloader_set_player_ready_cb`, re-added at the end of upstream's
  `answer_id()`/`run_dual_job()`), and nothing else.
- **delete/modify** (`git status` shows `UD`): upstream deleted the file and we
  changed it. Their rework is the reason it went (`podcastsaved.c` became
  `podcastcard.c`/`podcastopml.c`), so accept the deletion and move our edits —
  usually a card path or a size — into the new code.
- **docs** (`README.md`, `FEATURES.md`, `.github/`): `README.md` is ours
  entirely (it describes this fork); `FEATURES.md` takes upstream's and our
  four V3 deltas are re-applied on top; issue templates follow upstream.

Two things a merge cannot see, and both bite at link time rather than at the
conflict:

- a function **renamed** upstream and defined twice after the merge
  (`spin_start` in `spinner.c`, `smooth_sync`/`smooth_timer_sync` in
  `player.c`): keep one, the newer one, and fix the call sites;
- a file that uses `SC()` without including `gui.h`/`scale.h` — the compiler
  does not complain, the linker says `undefined reference to 'SC'`
  (`clocklabel.c`, `theme.c` in this merge).

## 3. Scale the new interface code

Upstream writes the interface at 480x720; this board compiles it at 320x480
through `SC()` (`src/gui/shell/scale.h`). Every upstream page arrives with the
480x720 numbers in it, and nothing about the merge scales them.

```sh
cd sonix-player
find src/gui -name '*.c' | xargs python3 tools/scale_gui.py --check   # what is bare
find src/gui -name '*.c' | xargs python3 tools/scale_gui.py --write   # wrap those in SC()
```

`--check` exits non-zero when it finds something, so it also works as a gate
before committing; `--write` only touches the arguments of the layout calls it
knows, and leaves every other line exactly as it was.

The script wraps bare integer literals in the arguments of the layout calls it
knows (`lv_obj_set_size`, `lv_obj_align`, the `pad`/`radius`/`border`/`shadow`
style setters...). It cannot do the rest:

- the `#define`d sizes at the top of a page are the bulk of it:
  `#define PILL_H 56` becomes `#define PILL_H SC(56)`;
- **what is not a size must not be wrapped**: counts, durations in
  milliseconds, percentages, loop limits — and numbers already in another
  space. `topbar.c`'s `BATTERY_CAVITY_*` are 24x24 SVG coordinates that
  `draw_battery()` scales itself; wrapping them shrank the level bar to two
  thirds of the cavity and shifted it a pixel towards the corner. The battery
  looked fine while charging, because the fill is hidden then.

Then look at it. The simulator is the only cheap way to catch a page that is
one number short:

```sh
make host                                       # in sonix-player/, on the host
SDL_VIDEODRIVER=offscreen SONIX_SD_ROOT=/tmp/sd \
  SONIX_LANG_DIR=$PWD/../sonix-packer/assets/BLAZE/usr/resource/sonix/language \
  ./sonix_player_host
```

- `SONIX_LANG_DIR` is not optional: without it every label shows its key
  (`date_and_time`), which reads like a broken merge and is not.
- a fresh `SONIX_HOST_STATE_DIR` re-enters the welcome flow, which is the quick
  way to see the first-run pages (Wi-Fi step, date and time, language).
- screen captures: `touch /tmp/sonix/shot`, the PNG lands in
  `$SONIX_SD_ROOT/Screenshots/`; taps and drags: `printf 'x y' > /tmp/sonix/tap`.
- the battery icon at a level, low and charging:
  `SONIX_BATTERY_CAPACITY=/tmp/cap SONIX_BATTERY_STATUS=/tmp/stat`, both being
  **paths to files** holding `64` and `Discharging` (or `Charging`).
- `make host` runs on the machine, **not** through `docker/build.sh host`: that
  image has no SDL and no ALSA headers.

## 4. Regenerate the small icon set

Upstream adds icons with its features (`metronome.png`, `year.png`,
`lyrics-search.svg`...). They reach the binary only through the generated
`icons_small.c`, and a missing one is an `undefined reference to
'icon_whatever'` at link time.

```sh
docker build -t sonix-icons -f sonix-player/docker/icons.Dockerfile .
docker run --rm -u "$(id -u):$(id -g)" -v "$PWD":/work \
    -w /work/sonix-player sonix-icons
```

`icons_small.c` and `icons_select.h` are then rewritten in the tree. Only the
small set: `icons.c` is upstream's 480x720 set, is not compiled here (it holds
the same symbols, which is why the Makefile compiles exactly one of the two),
and regenerating it would churn twenty-four megabytes for nothing.

## 5. Merge the language files

The interface's text lives in `sonix-packer/assets/BLAZE/usr/resource/sonix/language/`
and upstream's copies are in `assets/R3PII/.../language/` (same keys, same
files). Take their new keys three-way:

```sh
base=<previous tag>:sonix-packer/assets/R3PII/usr/resource/sonix/language
theirs=<new tag>:sonix-packer/assets/R3PII/usr/resource/sonix/language

for f in sonix-packer/assets/BLAZE/usr/resource/sonix/language/*.ini; do
  git show "$base/$(basename "$f")" > /tmp/base.ini
  git show "$theirs/$(basename "$f")" > /tmp/theirs.ini
  cp "$f" /tmp/merged.ini
  git merge-file -p /tmp/merged.ini /tmp/base.ini /tmp/theirs.ini > "$f"
done
```

`git merge-file` writes its conflict markers into the file and exits non-zero;
that is the expected half of this step. Ours carries the V3 strings (the AK4493
digital filters, the screensaver note); theirs carries the new keys. Where a conflict is one side empty, take the other;
where ours is the AK4493 block, keep ours and theirs both. A new language is a
new file copied whole (`Čeština.ini` in 1.3.0).

A key that is missing does not break anything — it shows on screen as the key
itself (`musicsettings_split_album_artists` did, on 11 of the 12 languages after
the 1.3.0 merge), which is how a half-merged language file is noticed. The merge
is what makes it happen: where a page had a key and ours did not, the three-way
merge sees "deleted on our side" and keeps the deletion, even though upstream
added it long ago.

So check the sets, and check them per file:

```sh
git show <new tag>:sonix-packer/assets/R3PII/usr/resource/sonix/language/English.ini \
  | sed -nE 's/^([a-zA-Z0-9_]+)[[:space:]]*=.*/\1/p' | sort -u > /tmp/up-keys

for f in sonix-packer/assets/BLAZE/usr/resource/sonix/language/*.ini; do
  sed -nE 's/^([a-zA-Z0-9_]+)[[:space:]]*=.*/\1/p' "$f" | sort > /tmp/k
  echo "$(basename "$f"): missing $(comm -23 /tmp/up-keys <(sort -u /tmp/k) | wc -l), \
duplicates $(uniq -d /tmp/k | wc -l)"
done
```

Both numbers must be zero — for every language, not only English. A duplicate
key is not harmless either: the loader keeps the **last** line of a key (it
prepends each entry to its bucket), so a merge that left an old translation
above a new one is a file where the reader sees the new one and the next reader
of the file sees both. 1.3.0 left 31 such pairs in `Japanese.ini`, all of them
with the upstream line second.

## 6. What is not taken from upstream, and what to check instead

- `sonix-packer/assets/*/etc/boot-logo/`, `etc/init.d/S11jpeg_display_shell`:
  the boot logo by theme. This board's logo comes from its kernel, and
  `/etc/logo.jpeg` does not exist here;
- `etc/init.d/S21mount_ubifs`: the HiBy boards' mount of `/usr/data`;
- `module_driver/*.ko`, the kernel itself, zram;
- upstream's packer script. Its new steps (the soname reshuffle in "room for the
  system libraries") matter for its own assets; ours are already in
  `assets/V3ANALOG/usr/lib`. Check that `verify` still passes.

Worth a look on every merge, because the packer installs these by name:
`assets/BLAZE/usr/resource/sonix/gui/` (file names changed?), `.../fonts/`,
the BlueZ userland in `assets/BLAZE/usr/{bin,sbin,lib}` and the libraries in
`assets/V3ANALOG/usr/lib`. `sonix-packer/tools/install_tree.sh` checks a list of them and
fails loudly when one did not land.

## 7. Build, and prove the ABI

```sh
sonix-player/docker/build.sh          # target build; check-abi runs inside
```

`check-abi` is not decoration: upstream develops against glibc 2.22, this device
has 2.16, and a symbol one release newer is a player that does not start — the
loader stops before `main` and the device looks bricked. When it fails, the fix
is a shim (`src/system/core/glibc_compat.h`) or not using the call.

## 8. Version, pack, flash

`OS_version` in `sonix-packer/assets/V3ANALOG/usr/resource/sonix/components/system-info.json`
is the **base** the player prints as "Operating system version", and the packer
appends the build's own name: a tag when HEAD is clean, a short hash otherwise.
Set the base to the upstream version being taken (`1.3.0`), without extra
components — the fork's own revision is the git tag.

```sh
STOCK=/path/to/v3_analog_original.upt sonix-packer/v3_analog_packer.sh
sonix-packer/v3_analog_packer.sh verify
```

Commit the merge, tag the release (`0.2.0`, `0.3.0`...), and **pack again after
the tag**: the image built before it carries the uncommitted-tree hash in About
instead of the tag. `git commit` is the repository owner's command, not an
agent's — stage, write the message, and hand it over.

Then flash it (stock firmware → firmware update from the card, or Power + Prev
into recovery) and check on the device, in this order, because each can hide the
next:

1. it boots, the interface is at 320x480 and nothing overflows;
2. the panel: scrolling does not freeze, the volume pill appears, the first
   frame after a wake is whole (the pan and the single page);
3. screen off and on through `SET_SLEEP`, then standby `mem` and its wake;
4. sound: 3.5 mm, 4.4 mm, the AK4493 filters, USB DAC mode (host must re-read
   descriptors only after a replug);
5. Bluetooth out and in, LDAC included;
6. memory: `MemTotal` is 56.5 MB. Upstream develops for a player with a
   gigabyte, so the new caches (covers up to 1000 rows, read-ahead, podcasts)
   are the first thing to measure — `Settings → Developer options → processes`
   and the log's own timings.

## 9. Afterwards

- `docs/V3-ANALOG.md`: the "Upstream base" line, and anything the release
  changed about this board (a new flag in `board.h`, a path that moved).
- `AGENTS.md` and `.opencode/NOTES.md`: a rule or a pitfall discovered on the
  way belongs there, in the same session.
- `FEATURES.md`: the feature list the release brought, with the V3 deltas.
