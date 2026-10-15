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

## Power: the backlight goes out, then the SoC sleeps

The screen-off on this device is the **backlight alone**. The stock player never
blanks the panel -- its binary has no `FBIOBLANK` in it at all, and dims
`/sys/class/backlight/pwm-backlight.0/brightness` to 0 instead -- and the port
follows it: `blank_panel = false` in the model, so no blank is ever issued and
the panel stays armed, scanning the frame it was last given. That is deliberate,
and it is what the freeze fix rests on: the state a blank leaves behind
(`LCDC_CTRL.ENA` back on, `SLCDC_CTRL.DMA_EN` still down) is the one this panel
does not come back from without a reboot.

The price is that the glass keeps showing the last frame. With the backlight off
an LCD's pixels still hold their state, so in ambient light the picture stays
readable; that is the panel, not a backlight that failed to go out.

The device does not stay awake for it. **Suspend-to-RAM is on by default**, 60
seconds after the screen goes dark, with the stock standby's own sequence: a
real, awaited stop of the audio pipeline, the radios parked, `sync()`, then
`mem`. `[power] standby_mem = 0` turns it off and the switch on the Power
settings page writes exactly that key; `[power] standby_mem_seconds` sets the
wait. On the way in the kernel powers the panel down, on the way out it runs
`lcd_power_on` and the panel's own table -- which is also the only power cycle
left that can free a panel that has stopped showing anything.

`[power] screen_off_seconds` is what starts that countdown: 30 seconds by
default, `0` for never, and the Screen page's slider is the same setting.

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
The link needs `-Wl,--allow-shlib-undefined`: the stock library's `ptn0`, `ptn3`
and `AS` are undefined and nothing in the firmware defines them -- nothing
references them either (no relocations), and the stock player lives with them.
Nothing is copied to the device: the library is the stock one, already in
`/usr/lib`.

**The library must be the only LDAC implementation in the process.** The
encoder side is built from AOSP's `libldac` (`libldacBT_enc`, `libldacBT_abr`),
and those libraries export the same `ldacBT_*` names as the stock combined one.
Both in one process means `ldacBT_get_handle()` resolves to one library while
the handle is used by the other's functions: different handle layouts, and the
receiver dies with SIGSEGV on the first real frame (the phone shows that as the
music stopping). So all three pkg-config files -- `ldacBT-enc.pc`,
`ldacBT-abr.pc` and `ldacBT-dec.pc` -- point at `-lldacBT`, the combined
library, which has the encoder too. `tools/ldacdec-test.c` is the small program
that shows the difference in isolation.

### The phone's LDAC encoder needs an MTU of 679 and up

BlueZ 5.66 announces no receive MTU for an AVDTP channel: `bt_io_accept()` on
the incoming media channel takes no options, and `l2cap_connect()` passes
`BT_IO_OPT_IMTU, 0` ("auto-tune"). An L2CAP configuration with no MTU option
means the L2CAP default, 672, and a phone feeding an LDAC stream hands its
encoder that value minus the 12-byte AVDTP media header: 660. libldac refuses
anything below 679, so the encoder is never initialized:

```
E/a2dp_vendor_ldac_encoder: error initializing the LDAC encoder: -1
    api_error = 1027 ... error_code = 0x40300000
I/a2dp_vendor_ldac_encoder: MTU=660, peer_mtu=660
```

`1027` is `LDACBT_ERR_ILL_MTU_SIZE`. What the user sees: the phone sends
`Start`, the AVDTP stream reaches `OPEN -> STREAMING`, no audio arrives at all,
and then the phone suspends or drops the link -- which it also does with SBC
when nothing drains the transport.

`tools/bluez-5.66-avdtp-transport-mtu.patch` fixes it: `AVDTP_TRANSPORT_IMTU`
(1024, what a phone announces for its own side) is announced on both paths that
create an AVDTP channel -- the accepted media channel and the connected one.

With both fixes in place the receiver runs at real time, measured on the device
(2026-10-13) with `tools/rxprobe.c` reading the PCM:

```
rxprobe: at 5 s: 125685 frames, 2.85 s of audio
rxprobe: at 10 s: 346185 frames, 7.85 s of audio
rxprobe: at 15 s: 567126 frames, 12.86 s of audio
```

`analysis/v3-bt-scripts/` holds the stand the runs were made with (headless
receiver, HCI capture, phone driven over adb) and `tools/btadm.c` is the D-Bus
tool it needs: adapters, discovery, pairing agent, connect, and a dump of
bluealsa's transports.

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

Recovery can also be asked for from a shell, which is how the update menu does
it and how a session over adb flashes without touching the device:

```
echo recovery > /proc/jz/reset/reset
```

The SoC then boots the recovery kernel, which takes `v3_analog.upt` off the card
and writes it. On the device this took about 75 seconds, adb returning with the
new build. Nothing else has to be written first, and one thing in particular
must not be:

**`/data/recovery_all` is a factory reset, not part of the update.** The init
script `S39_recovery.recovery` reads that file on *every* boot and runs
`recovery_all.sh`, which clears `/var/lib/bluetooth`, `/var/lib/dbus`,
`/usr/var` and then `rm -rf /data/*`: the settings, the Bluetooth pairings, and
whatever else lives there. The word inside chooses how much: `recovery_all`
erases everything, `recovery_data` spares only the stock firmware's
`usrlocal_media.db`. The stock binary's strings put the two next to each other,
which is how a firmware update came to be run with a factory reset attached --
see `sonix-player/src/system/device/factoryreset.c`, which documents the whole
mechanism, and the `v3-adb-flash` skill. (The player's own media database is on
the card, in `.local/library.db`, and survives either way.)

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

Verified on the device (2026-10-13): each slider takes effect on its own
output, and moving one no longer moves another.

## What does not work, and what is not verified

Not working, with the reason:

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
- **suspend-to-RAM**: a screen-off left alone for a minute writes `mem`, and the
  wake comes back with the panel armed and scanning (TE 76 Hz), the touch
  controller alive and no instant wakes in the log;
- Bluetooth: pairing, AVRCP volume, A2DP out, A2DP in (SBC, AAC, aptX,
  aptX-HD), and **LDAC out** (confirmed by ear on LDAC headphones, not only by
  the player's own codec report);
- **LDAC in**: fixed and verified on 2026-10-13 -- a phone's LDAC stream runs at
  real time through the receiver (`rxprobe` numbers in the LDAC section above).
  Two defects had to go: BlueZ announcing no receive MTU for the media channel
  (the phone's encoder refused 660 against LDAC's 679 minimum) and two LDAC
  implementations in one process (the AOSP encoder libraries next to the
  stock combined one). Both fixes are in
  `tools/bluez-5.66-avdtp-transport-mtu.patch` and in the bluealsa build
  (all three LDAC pkg-config files point at the combined library). Verified
  headless, with the receiver's userland driven from the card; hearing it
  through the player's own receiver page is the last check.
