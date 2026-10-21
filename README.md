# Sonix Player for the TempoTec V3 Analog

A port of [Sonix Player](https://github.com/Jepl4r/sonix-player) by
[@Jepl4r](https://github.com/Jepl4r) to the **TempoTec V3 Analog** — the
2024 `v3_analog` firmware generation (`VERSION.TXT`: `name=v3_analog`,
`ver=2024-12-24T11:00:26+08:00`), not the later **V3 Blaze**
(`V3_ANALOG_2025`). It replaces the stock music player and keeps TempoTec's
kernel, drivers and recovery.

This port is built on the [Blaze port](https://github.com/carmine-bin/sonix-player-tempotec-v3-blaze)
(`carmine-bin/sonix-player-tempotec-v3-blaze`, branch `blaze-port`) as its
ancestor: the two players share the panel size (320x480) and the AK4493SEQ DAC,
so the interface and the audio board came across. Underneath, the V3 Analog is
the older hardware — glibc 2.16, a 32-bit SLCD panel reached through the
vendor's `FBIOPAN_DISPLAY` and `SET_SLEEP` ioctls, the codec on ALSA card 1, and
a BlueZ 5 userland built for it. What changed and what is not done yet is in
[docs/V3-ANALOG.md](docs/V3-ANALOG.md); the display path and the panel power
are in [docs/DISPLAY.md](docs/DISPLAY.md) and
[docs/PANEL-POWER.md](docs/PANEL-POWER.md), Bluetooth in
[docs/BLUETOOTH.md](docs/BLUETOOTH.md).

Flashing always carries some risk: read
[Going back to stock and recovery](#going-back-to-stock-and-recovery) first.

The upstream README, with the full build documentation and the source tree, is
in [UPSTREAM-README.md](UPSTREAM-README.md). Features are in
[FEATURES.md](FEATURES.md); building this image end to end is
[docs/BUILD.md](docs/BUILD.md).

The images this repository builds carry no streaming credentials: Tidal, Qobuz,
podcasts and Last.fm need your own account or keys; see [Streaming](#streaming).

## Install

1. Charge the player above 30 %.
2. **Keep a copy of the official firmware.** If the root of your microSD
   already has a `v3_analog.upt` (the stock OTA leaves one there), rename it
   to `v3_analog.upt.stock` and copy it to your computer as well. Remove any
   `update.upt`: the stock updater prefers it over every other name.
3. Get `v3_analog.upt` — build it yourself as [docs/BUILD.md](docs/BUILD.md)
   describes. It is a modified copy of the vendor's firmware and is deliberately
   not published here, so there is nothing to download.
4. Copy it to the **root of the microSD card**, keeping the name
   `v3_analog.upt`.
5. Start the update:
   - **from the stock firmware**: Settings → Firmware update → Update via
     micro SD card;
   - **from Sonix**: Settings → System → Update firmware → From SD card.
6. Let it flash and reboot by itself.

The name matters: the player looks for its own model's file and refuses every
other one, `update.upt` included (`sonix-player/src/system/device/firmware.c`).
The recovery route is the same file — Power + Previous at power-on, then let
the updater take `v3_analog.upt` off the card.

## Updates

This port does **not** offer updates over the internet. The update page has
"From SD card" only: `sonix-packer/assets/V3ANALOG/usr/resource/sonix/components/system-info.json`
carries no `ota-repo`, and `fwupdate.c` hides the online button when the key is
absent — a port that publishes no releases would otherwise ask the default
repository, whose releases are built for other players. A new build is flashed
the way the first one was.

## Versions

About's **Operating system version** reads `<base>/<build>`. The base is the
`OS_version` in `system-info.json` up to the first `-` or `/` (today
`1.2.0.1`); the build is the tag on HEAD, `#<short hash>` for a commit without
one, or eight hex digits of an md5 of the uncommitted changes for a test build.
The packer writes it per build (`os_version()` in
`sonix-packer/v3_analog_packer.sh`); the table is in
[docs/V3-ANALOG.md](docs/V3-ANALOG.md#the-version-the-player-reports).

## What this port brings

Most of the changes below are the Blaze port's own and this port inherits them,
because it started from that tree. The numbers were measured on the Blaze
unless a document says otherwise; what is specific to the V3 Analog — the
display path, panel power, the codec's card, the Bluetooth stack, USB DAC mode
— is in [docs/V3-ANALOG.md](docs/V3-ANALOG.md).

**Hardware**

- The pages in `sonix-player/src/gui` are authored at 480x720 and every
  absolute size and position goes through `SC()` (`sonix-player/src/gui/shell/scale.h`),
  which is two thirds of the number for this board at compile time: the
  interface is drawn at the panel's own 320x480, not laid out large and
  shrunk. The touch panel follows, since evdev maps its range onto the
  display's resolution.
- AK4493SEQ audio board: three gain levels like the stock player (LDB, MDB,
  HDB), TempoTec's default DAC filter, the analogue output muted before
  power-off and reboot, the amplifier left off until the first track.
- The hardware buttons the right way round, the button list page. The boot
  logo stays TempoTec's, drawn from the `logo` partition: this port does not
  write it (`one_boot_logo = false` in `board.h`).

**Bluetooth**

- `main.conf` with `ControllerMode = bredr` and `FastConnectable = false`
  (`sonix-packer/assets/V3ANALOG/usr/etc/bluetooth/main.conf`): the receiver
  range fix from the
  [V3 Blaze mod](https://github.com/carmine-bin/tempotec-v3-blaze-mod)
  (LDAC 990 kbps holds).
- Receiver: aptX HD decoded correctly (it used to be noise).
- Receiver: a paired phone comes back without pairing again. Opening the
  receiver invites every paired phone or computer, starts on the best codec
  the phone offers (LDAC first), and lets go of it when the receiver closes.
  Tapping a phone in the list links it without audio.
- The stack is BlueZ 5.66 + bluealsa 4.0.0, built for this device's glibc
  2.16, in place of the stock BlueZ 4.101; the defects that had to be fixed
  for LDAC receive and aptX HD are in [docs/BLUETOOTH.md](docs/BLUETOOTH.md).

**Speed and battery**

- Covers: a persistent RGB565 cache, decoded off the interface thread; the old
  cover stays up until the next is ready, and follows a skip at once.
- Skips: the audio output stays open, and the next tracks' tags, favourite
  star and glyphs are ready before the press (skip-to-sound about 46 ms).
- Lists: fewer database reads while scrolling, the first rows bound first,
  the letters of the library's names warmed in the background.
- Start-up: OpenSSL and the code pinning off the path to the first frame.
- Waking the screen re-runs the panel's own resume — rails, init table, pan,
  then a full repaint — the sequence the stock player uses. There is no
  backlight-only fast path on this board: the panel was really powered down
  (see [docs/PANEL-POWER.md](docs/PANEL-POWER.md)).
- Hidden spinners, marquees and carets stop; with the screen off and music
  playing the interface wakes about 7 times a second instead of 44.
- On this board, suspend-to-RAM 60 s after the screen goes dark, with the
  stock standby's own sequence; `[power] standby_mem = 0` turns it off.

**Interface**

- AMOLED theme beside Dark and Light.
- Now playing: Show artist, beside Show album.
- About names the DAC as the AK4493SEQ.

The Blaze port's own full list is the commit history of its
[`blaze-port`](https://github.com/carmine-bin/sonix-player-tempotec-v3-blaze/commits/blaze-port)
branch.

## Going back to stock and recovery

**Back to stock:** put the official firmware in the card root as
`v3_analog.upt` and install it from Settings → System → Update firmware →
From SD card, or through recovery. The official builds are on
[TempoTec's firmware page](https://www.tempotec.net/pages/firmware-download):
take the V3 Analog's own, not the V3 Blaze's and not the older V3's. The one
this port is developed against is build **1.3**
(`ver=2024-12-24T11:00:26+08:00`), and the packer refuses any other stock
`.upt` whose `UBOOT.BIN` it does not know (see
[docs/V3-ANALOG.md](docs/V3-ANALOG.md#the-image-v3_analogupt)).

**If the player does not boot:** with the player off, hold **Power + Previous
track** past the TempoTec logo until **updater** appears. A valid
`v3_analog.upt` in the card root is flashed without asking, so check which
file is there first. The image only writes the main kernel and rootfs; the
bootloader and the recovery system are never touched.

## Building

The player builds with the project's own MIPS toolchain and the packer wraps it
in a firmware image; the whole recipe, from a fresh clone to
`sonix-packer/out/v3_analog.upt`, is in [docs/BUILD.md](docs/BUILD.md). The
toolchain side — `make target` in `sonix-player/`, the Rockbox MIPS
toolchain, the glibc 2.16 handling — is in
[docs/V3-ANALOG.md](docs/V3-ANALOG.md#building-for-the-target), and the
upstream build documentation in
[UPSTREAM-README.md](UPSTREAM-README.md).

The packer starts from the **vendor's own 1.3 firmware**, which is not
redistributed here. It checks that the bootloader inside it is the stock one
(`UBOOT.BIN` md5 `1f54858d6b4e6f482a81a3378c0e5bf8`) and carries the stock
kernel (`UIMAGE.BIN` md5 `35889275f8bed2f967a18f6a8b66495c`) through untouched:

```
sonix-packer/
├── v3_analog_packer.sh
├── v3_analog_original.upt   the vendor's 1.3 UPT, put here by hand
├── sonix_player             from make target
├── sonix_launch             from make target
├── assets/BLAZE/            the player's interface assets, shared by the models
└── assets/V3ANALOG/         and what is this device's own, over them
```

`./v3_analog_packer.sh` writes `out/v3_analog.upt`. It needs docker for the
tools it runs (`7z`, `mkfs.ubifs`, `genisoimage`, `ubi_reader`), built from
`sonix-packer/docker`.

## Streaming

The accounts live on the card, not in the firmware: Qobuz and Tidal tokens go in
`<card>/sonix/streaming.json`, and the player keeps what it renews in
`<card>/sonix/streaming-session.json` next to it
(`sonix-player/src/system/streaming/streamkeys.h`). No Qobuz
application credentials are compiled in — the player derives them from Qobuz's
web player when it first needs them and caches them in the session file; Tidal's
client pair is built in. Podcast Index and Last.fm keys come from a sealed
`streaming-keys.bin` in the rootfs, and this repository does not ship one.
Without a service's credentials it disables itself and says so, instead of
failing halfway through a login. The full picture is in
[docs/BUILD.md](docs/BUILD.md#7-streaming-services).

## License and credits

GPL-3.0, like upstream ([LICENSE](LICENSE)). Sonix Player is the work of
[@Jepl4r](https://github.com/Jepl4r) and its contributors; see the special
thanks at the end of [UPSTREAM-README.md](UPSTREAM-README.md). The Blaze port
this one is built from is by [@carmine-bin](https://github.com/carmine-bin).
Not affiliated with TempoTec or HiBy.
