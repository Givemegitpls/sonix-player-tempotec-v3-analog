# Sonix Player on the TempoTec V3 Analog

This is a fork of the
[Blaze port](https://github.com/carmine-bin/sonix-player-tempotec-v3-blaze)
(`carmine-bin/sonix-player-tempotec-v3-blaze`, branch `blaze-port`), adapted to
the **TempoTec Variations V3 Analog** (the 2024 `v3_analog` firmware
generation, not the `V3_ANALOG_2025` Blaze). Upstream Sonix Player is
[Jepl4r/sonix-player](https://github.com/Jepl4r/sonix-player); the full build
documentation and the source tree are in
[UPSTREAM-README.md](../UPSTREAM-README.md).

The Blaze and the V3 Analog share the glass and the DAC, so the Blaze port is
the base. What follows is what had to change underneath: a different SoC
revision's glibc, a panel that wants 32 bits a pixel and one page, a codec on
another ALSA card, and a Bluetooth stack that is not the port's.

## The device

- **SoC**: Ingenic X1000E (MIPS32r2, one core), **Linux 3.10.14**, Buildroot
  2014.05, **glibc 2.16**. There is no compiler on the device.
- **RAM**: 56.5 MB (`MemTotal` 57816 kB; the spec's 64 MB is the part the
  kernel and CMA keep).
- **Display**: 320x480, `32 bpp` in `[B, G, R, A]` order, driven through an
  **SLCD** panel.
- **Audio**: two **AK4493SEQ** DACs behind the 3.5 mm and 4.4 mm outputs.
- **Storage**: the card is mounted at `/mnt/sd_0`; the one writable volume is
  `/data` (a UBIFS volume of its own). The library lives on the card only.
- The spec sheet overstates several things (the screen, PCM rates, RAM). The
  device is the source of truth.

## Building for the target

The player is built with the project's own MIPS toolchain,
`mipsel-rockbox-linux-gnu` (binutils 2.38, gcc 9.5.0, headers 3.10.108,
**glibc 2.27 configured with `--enable-oldest-abi=2.16`**), the same one the
Blaze port uses. `make target` in `sonix-player/`.

The device's glibc is **2.16**, so the Makefile sets:

```
DEVICE_GLIBC_MINOR = 16
```

The toolchain's glibc 2.27 moved `clock_gettime` out of `librt` and into
`libc`, and references it as `GLIBC_2.17` -- a version the device's libc does
not export at all. The device's `librt.so.1` does export it, as `GLIBC_2.2`,
so `glibc_compat.h` asks for that name:

```c
__asm__(".symver clock_gettime,clock_gettime@GLIBC_2.2");
```

and `TARGET_LDFLAGS` adds `-lrt`. Without both halves the binary links and
then does not start: the loader stops on ``version `GLIBC_2.17' not found`` and
the device boot-loops.

`make check-abi` walks the binary's versioned symbols and refuses anything
newer than the device's glibc. For this port it reports no symbol beyond
glibc 2.16; the highest actually present is **`GLIBC_2.9`**.

## Display: 32 bits, one page, a 2/3 blit

The panel is an SLCD, and a frame reaches the glass only through
**`FBIOPAN_DISPLAY`**: the vendor driver swaps the descriptor and restarts the
controller's DMA. Writing to the framebuffer memory alone changes nothing
visible.

The Blaze port requires a 16-bit page with a tight stride and falls back to
plain fbdev otherwise. The V3 Analog's page is **32 bpp BGRA**, so `main.c`
grew a second path:

- `try_panned_display()` accepts 2 and 4 bytes a pixel (still tight-stride
  only) and counts the pages the driver already has
  (`smem_len / (line_length * yres)`).
- LVGL draws RGB565. At 32 bpp it always draws the **480x720 layout into a RAM
  page**, and the copy rewrites it into the panel's BGRA: `fb_store_pixel()`
  widens the fields (5->8, 6->8, full scale kept full scale) and writes one
  32-bit word per pixel, opaque.
- The interface is laid out at **480x720 and shrunk 2/3** onto the 320x480
  glass (the same 9/3/3/1 filter the Blaze port uses), because the panel is two
  thirds the size of the layout.

**One framebuffer page** (`single_fb_page` in the model, `true` here). The
driver does offer two pages, but on this panel:

- with two, the frame on the glass is always the one drawn **two frames ago**
  -- the volume pill appears on the next scroll, and nothing else appears
  either;
- and each page owes the other everything that changed while it was off
  screen, so every frame pays a **full-screen copy (about 26 ms)**.

With one page and a pan, only the frame's own damage is copied: **about 2 ms**
for the same frame. `single_fb_page` keeps the Blaze/R3 Pro II path
byte-for-byte.

A blank or unblank rewrites the mode underneath the player: the driver resets the
virtual size and the descriptor when the panel powers down, and from then on the
`yoffset` that was right before is refused with `EINVAL` -- every frame, for ever.
On the device that is a lit screen showing the last frame that ever reached it
while the interface behind it keeps drawing: a frozen picture, a clock that
stopped, an indicator still saying what was true minutes ago. The pan is retried
once after re-reading the mode (`fb_mode_stale`), which is all it takes.

## Sound: card 1, and the analogue output is `plughw:1,0`

Card 0 on this device is `sa_spdif`, a dummy the optical route shares. The
codec's mixer controls and the analogue output are on **card 1**:

- the mixer (`AK4493 Gain Control`, `Digital Filter`, `Output Port Switch`, the
  playback volumes) is opened as **`hw:1`** (`mixer_card = 1`);
- the analogue output is opened as **`plughw:1,0`** (`pcm_device`), because
  `default` resolves to card 0 here. `plughw` and not `hw`: the codec takes a
  narrow set of formats and rates, and the plug keeps a 24/96 track from being
  refused outright until exactly what it takes is measured.

Both are model fields (`sysinfo.h`), so the Blaze and the HiBy boards keep
their `hw:0` and `default`.

## Bluetooth: a BlueZ 5 userland on a glibc 2.16 device

The port's Bluetooth layer and bluez-alsa are written against **BlueZ 5**
(`org.bluez.Adapter1`, `Media1`, `RegisterPlayer`, `GetManagedObjects`). The
stock firmware carries **BlueZ 4.101** (`org.bluez.Adapter`, `Audio`,
`Manager`), whose API is not compatible. Bringing the port's Bluetooth up means
replacing the userland.

The controller itself is fine: the stock `bt_init`
(`brcm_patchram_plus` + `hciconfig hci0 up`) binds the chip over UART and
exposes `hci0`. Only the daemon and the audio path are replaced.

### The build recipe

Everything is cross-compiled under `mipsel-rockbox-linux-gnu` with
`-O2 -march=mips32r2`, and linked **against the device's own libraries** -- the
same trick as glibc: the device's `glib` 2.46.1, `dbus` and `alsa` 1.0.26 are
what the binaries load, so they are what the linker must see. Headers of
suitable versions come from Debian's mipsel packages (`glib` 2.42.1,
`dbus` 1.10.32).

- **sbc 2.1** (SBC moved out of the BlueZ tree in >= 5.5).
- **bluez 5.66** -- `bluetoothd` and `libbluetooth.so.3`, configured with
  `--enable-library --disable-client --disable-systemd --disable-udev
  --disable-cups --disable-obex --disable-network --disable-hid --disable-hog
  --disable-bap --disable-mcp --disable-vcp --disable-tools --disable-monitor`
  and `RST2MAN=/bin/true` (otherwise `configure` demands the man pages).
  `--disable-client` drops the readline requirement.
- **bluez-alsa 4.0.0** -- not 4.3.1, which already requires glib 2.58.2 while
  the device has 2.46. Configured with `--disable-aplay --disable-systemd
  --disable-upower`.
- **Codecs**: `fdk-aac` (AAC), `libopenaptx` (aptX / aptX-HD), and AOSP's
  `libldac` (LDAC; encoder and ABR). bluez-alsa 4.0.0 knows `libopenaptx` only,
  so it is enabled with `--with-libopenaptx`, and the codec flags
  `--enable-aac --enable-aptx --enable-aptx-hd --enable-ldac`.
- **`tools/glibc216_shim.c`** is compiled into `bluetoothd` and into the ALSA
  plugin: `getrandom` (its syscall arrived in Linux 3.17; this device runs
  3.10, so the shim reads `/dev/urandom`) and `clock_gettime` (defined locally
  so the versioned `GLIBC_2.17` reference leaves the binary entirely). Nothing
  else in either binary asks for anything newer than 2.16.

### What has to be on the device

- `/usr/share/alsa/alsa.conf.d/20-bluealsa.conf` -- the file that declares the
  `bluealsa` PCM name. Without it alsa-lib says "Unknown PCM bluealsa", even
  with the plugin in place.
- A D-Bus policy that lets bluealsa own its name:
  `<allow own="org.bluealsa"/>`.
- **`ControllerMode = bredr`** in bluetoothd's config. On the 3.10 kernel the
  LE/GATT path does not come up: BlueZ registers the adapter, fails to create
  the GATT database, and removes it again -- zero adapters on the bus. In
  BR/EDR mode no GATT database is built and the adapter registers.
- The BlueZ config is read from **`/usr/etc/bluetooth/main.conf`**, not
  `/etc/bluetooth/main.conf`: the build uses `--prefix=/usr`, so `sysconfdir`
  is `/usr/etc`. A config in `/etc` is silently ignored and the daemon starts
  in dual mode and falls into the GATT failure above.

## USB audio (DAC mode): the firmware's own gadget

The Blaze port builds a configfs gadget (`/sys/kernel/config/usb_gadget/android0`)
and creates a UAC function in it. The V3 Analog's kernel does not offer that
function there at all (`mkdir .../functions/uac_sa.a` fails with
`Function not implemented`), but it does have the same UAC function in its
**legacy** android USB gadget -- `/sys/class/android_usb/f_uac_sa`, which is what
`/dev/uac_sa` belongs to. The stock player switches modes by writing into that
gadget, and DAC mode is one of them:

```
DAC/Audio:  idVendor=22E1 idProduct=F008 functions=uac_sa,hid
Storage:    idVendor=22E1 idProduct=0101 functions=mass_storage
ADB/Dock:   idVendor=18D1 idProduct=0D02 functions=adb
```

`usbdac.c` grew a second path for it (`usb_gadget_legacy`, `usb_audio_vendor`,
`usb_audio_product`, `usb_audio_functions` in the model): disable the gadget,
write the ids and the function list, enable it again, and put the previous mode
back on the way out. The function list can only be written while the gadget is
disabled; a write that is refused silently leaves the old list in place.

Two things to know when testing it: the **host re-reads the descriptors only
after the cable is replugged** (the pullup cycle alone was not enough here), and
**ADB is unavailable while DAC mode is on** -- `adb` is not in the function list,
exactly as on the stock firmware. Leaving the page restores the previous mode;
a reboot restores everything.

Verified on the device: the host sees `22e1:f008 TempoTec V3` and a
`[V3]: USB-Audio - V3` sound card, and audio plays through the player.

## LDAC receive, from the firmware's own library

Newer AOSP `libldac` ships the encoder and ABR only; the decoder was removed, so
a decoder has to come from somewhere else. It is already on the device: the
stock firmware's `/usr/lib/libldacBT.so` is a **combined** library (encoder,
decoder and ABR in one) that HiBy's own Bluetooth plugin loads, and its ABI is
`GLIBC_2.2` at most with `libc` as its only dependency -- it runs on this
device's glibc 2.16 as it is.

Enabling it is a build change only: put that `.so` in the cross sysroot, add the
two decode prototypes to `ldacBT.h` (the AOSP header has the encoder only) and an
`ldacBT-dec.pc` (`Libs: -lldacBT`), then rebuild bluealsa with `--enable-ldac`.
The link needs `-Wl,--allow-shlib-undefined`: the stock library has unresolved
`ptn0`, `ptn3` and `AS` symbols that nothing in the firmware defines -- they are
not called (lazy binding), and the stock player lives with them. Nothing is
copied to the device: the library is the stock one, already in `/usr/lib`.

The library is in place and bluealsa does negotiate LDAC as a sink -- the phone
picks it, the endpoint is configured, and the daemon starts its decoder thread:

```
bluealsa: D: bluez.c:313: A2DP Sink (LDAC) configured for device ...
bluealsa: D: a2dp-ldac.c:334: IO loop: START: a2dp_ldac_dec_thread
```

and no audio comes out of it. Measured on 2026-10-13, with `bluetoothd` running
at debug level and a client reading the capture PCM:

- the AVDTP stream reaches `OPEN -> STREAMING` (the phone sends `START_CMD`), the
  transport becomes `ACTIVE`, and then the controller drops the ACL link:
  `dev_disconnected() Device ... disconnected, reason 3` -- HCI hardware
  failure. The phone reconnects and the cycle repeats, which is what the phone
  shows as the music pausing every few seconds;
- bluealsa is idle while this happens: 0.44% of a core over a 45 s attempt, so
  the decoder is not too slow;
- the same setup does receive aptX-HD, and the link survives longer the lower
  the bitrate -- SBC/AAC in the player's receiver, aptX-HD for a couple of
  seconds, LDAC not at all.

So the failure scales with the bitrate and ends with the controller reporting a
hardware failure, not with anything bluealsa logs. Two candidates are left, and
both need a run on the device:

- **Coexistence with Wi-Fi.** `wlan0` is associated here, and this is a combo
  chip with one antenna. Test: `ifconfig wlan0 down`, then receive LDAC.
- **The stock stack's own LDAC handling.** The firmware's `bluetoothd` is BlueZ
  4.101 with `a2dp_ldac_support`, `ldac_getcap_ind` and `ldac_setconf_ind` in
  it: on this board LDAC has always been a BlueZ 4 feature. If BlueZ 5.66 plus
  bluealsa is missing an AVDTP step the phone expects, the phone is the side
  that gives up.

`tools/rxprobe.c` is the instrument for this: it opens the receiver's PCM,
reads as fast as the daemon produces, re-opens it when a transport dies, and
prints the frames and the audio seconds it got.

## The image: `v3_analog.upt`

The port is a firmware, not a folder on a card: `sonix-packer/v3_analog_packer.sh`
builds an image that boots it, taking the stock firmware as the starting point.

```
cd sonix-packer
STOCK="../tempotec v3/v3_analog.upt" ./v3_analog_packer.sh
```

What it does, in order: unpacks the stock `.upt` (an ISO) and extracts
`SYSTEM.UBI` (the UBIFS rootfs) from it; deletes `usr/bin/hiby_player` and
installs `sonix_player`; installs the launcher (`usr/bin/sonix_player.sh`) and
`sonix_launch` and points `etc/init.d/S92_03_start_music_player` at them; copies
`assets/BLAZE/` -- the player's own interface, its fonts, languages, images and
Bluetooth userland -- and then `assets/V3ANALOG/` over it; writes the build
stamp into `system-info.json`; deletes the stock interface's own resources
(`litegui`, `layout`, `str`, `fonts`); repacks the rootfs with `mkfs.ubifs`
(`-c 480 -e 0x1F000 -m 0x800`, the geometry of this device's rootfs volume),
recomputes the rootfs digest in `UPDATE.TXT`, and rebuilds the ISO with
`genisoimage`. Then it unpacks its own result and checks it: the digest, the
files, their modes, and that the Bluetooth userland in the image is the one the
assets carry and not the stock's.

`UBOOT.BIN` is carried across untouched, deliberately: writing the bootloader is
the one way to brick this device, and the update route does not need it.

Two things the verify step exists for, both learned the hard way:

- **The executable bit on the init script.** The packer rewrites
  `etc/init.d/S92_03_start_music_player` in place, and a rewrite that drops its
  mode leaves a firmware whose init skips it -- busybox init does not run a
  script without the bit. The player then never starts, and what stays on the
  screen is the boot logo, which the bootloader draws from the `logo`
  partition, not the player. It looks exactly like a player that hangs. The
  rewrite preserves the mode and the check refuses an image where any of
  `S92_03_start_music_player`, `S98adb_debug`, `sonix_player`,
  `sonix_player.sh` or `sonix_launch` is not executable.
- **Adb when the player is not there.** On this firmware adb is a USB mode that
  only the player selects (the stock has no script that starts it at boot), so a
  player that does not come up leaves no adb either, and nothing to look at. The
  image carries `etc/init.d/S98adb_debug`, which puts the gadget into adb mode
  and starts `adbd` whenever neither `sonix_player` nor `sonix_launch` is
  running, and stays out of the way while the player runs -- the player owns the
  gadget's mode otherwise (DAC mode, storage mode). `/mnt/sd_0/disable_adb`
  pauses it.

The tools live in the docker image the sibling project builds
(`docker build -t tempotec-fw-tools "../tempotec v3/docker"`): 7z, ubi_reader,
mkfs.ubifs and genisoimage, none of which this host has.

The result is `out/v3_analog.upt`. To flash it: copy it to the card **as
`v3_analog.upt`** -- the name is what the recovery looks for -- and enter
recovery with Power + Prev. What it replaces is the rootfs; `/data` (the
settings and the media database) and the card are not touched.

What the image carries and the stock firmware does not: BlueZ 5.66's
`bluetoothd` and `libbluetooth.so.3` (the stock is BlueZ 4.101), `bluealsa` with
the AAC, aptX, aptX-HD and LDAC codecs, the ALSA plugin and its
`usr/share/alsa/alsa.conf.d/20-bluealsa.conf`, the D-Bus policy that lets
bluealsa own its name, `usr/etc/bluetooth/main.conf` with
`ControllerMode = bredr` (the stock config in `/etc` is ignored: this BlueZ is
built with `--prefix=/usr`), and a `usr/bin/bt_init` that only attaches the
controller -- the stock one also starts BlueZ 4's `bt-agent` and `bt-monitor`,
which cannot talk to a BlueZ 5 daemon, and doing the attach twice is what leaves
this controller with no index at all.

Because the resources are inside the image, the bind mount the port needed while
it lived on the card
(`mount --bind /mnt/sd_0/sonix-res/usr/resource/sonix /usr/resource/sonix`) is
not part of the picture any more, and neither is the card being there at boot.

## Volume limits, one per output

The board has three ways out, and the stock firmware caps each on its own: the
3.5 mm single-ended jack, the 4.4 mm balanced one, and the USB-C port in DAC
mode. The port's Settings → Music → Volume limit has a slider per output, and
the caps are meant to be independent: a ceiling chosen for headphones must not
follow the user to the line out, or to a PC on the other end of the cable.

Getting that wrong once produced two symptoms at the same time, and both came
from one line: the routing watcher classified the current output by the PCM's
name and treated anything starting with `plughw:` as the USB port. On this board
the analogue output *is* `plughw:1,0` (card 0 is `sa_spdif`, the optical dummy),
so the profile was always "USB". Its ceiling was then the only one ever enforced
-- and it was enforced through the codec's attenuation, which is what the jacks
are plugged into -- while the 3.5 mm and 4.4 mm sliders stored a value that
nothing applied. The values were never shared; the *classification* was.

The classification now lives where the outputs are known (`resolve_output()` in
`alsa-controls.c`): the USB port is `usbaudio_active()`, and with no USB card
attached the route is whichever jack is in (`headphone_jack_state()`, giving
balanced or phones). Each ceiling is applied on its own route -- the jacks
through the AK4493's `Left/Right Playback Volume` on `hw:1`, the USB port
through its own volume control or, when the USB card has none, a software
multiplier -- and each slider writes only its own key in `device_config.ini`
(`volume_limit_phones`, `volume_limit_balanced`, `volume_limit_usb`).

The stock player names its ceilings the same way and applies them at their own
outputs (`USB_OUT_MAX_VOLUME`, `SPDIF_MAX_VOLUME`, `DOP_MAX_VOLUME` in
`hiby_player`); it never had this bug because it never collapsed the ceilings
onto one "current output".

Verified in code and by build; not yet verified on the device.

## What does not work, and what is not verified

Not working, with the reason:

- **LDAC receive**: the phone negotiates LDAC, bluealsa starts its decoder, and
  the controller then drops the link with a hardware failure before any audio
  arrives. The measurements and the next two tests are in the LDAC section
  above. SBC, AAC, aptX and aptX-HD receive all work.
- **Charge limit**: the AXP2101 is not reachable over I2C here (`/dev/i2c-0`
  does not exist); the stock firmware goes through
  `/sys/class/axppower` and `/sys/class/power_supply/usb` instead. Until that
  is written, the switch is off rather than pretending.
- **Serial number in About**: the device's node is
  `/proc/jz/efuse/efuse_serial_nb`, not the `efuse_chip_id` the port reads.
- **Optical / S/PDIF**: the route to card 0 (`sa_spdif`) on the `digital_dt2`
  switch is not implemented.
- **Smoothness of page opens**: the first frame of a new page takes 96-135 ms
  (widget building, font rasterisation, the blurred backdrop); scrolling is at
  about 45 fps. Profiling and the cheap wins are still to be done.

Verified on the device:

- the physical keys, including `media_keys_swapped = true` (the kernel calls the
  upper skip key `KEY_NEXTSONG`, so it has to be read as "back", as on the
  R3 Pro II);
- sound by ear: wired 3.5 mm and 4.4 mm, Bluetooth out (A2DP), Bluetooth in
  (receiver), and optical;
- screen-off and wake through the framebuffer blank node, with the pan fix above;
- Bluetooth: pairing, AVRCP volume, A2DP out, A2DP in (SBC, AAC, aptX,
  aptX-HD), and **LDAC out** (confirmed by ear on LDAC headphones, not only by
  the player's own codec report). LDAC **in** does not work yet -- see the LDAC
  section above.
