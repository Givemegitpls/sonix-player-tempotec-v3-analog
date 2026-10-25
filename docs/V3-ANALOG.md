# Sonix Player on the TempoTec V3 Analog

This is a fork of the
[Blaze port](https://github.com/carmine-bin/sonix-player-tempotec-v3-blaze)
(`carmine-bin/sonix-player-tempotec-v3-blaze`, branch `blaze-port`), adapted to
the **TempoTec Variations V3 Analog** (the 2024 `v3_analog` firmware
generation, not the `V3_ANALOG_2025` Blaze). Upstream Sonix Player is
[Jepl4r/sonix-player](https://github.com/Jepl4r/sonix-player); the full build
documentation and the source tree are in
[UPSTREAM-README.md](../UPSTREAM-README.md).

The Blaze and the V3 Analog share the panel size (320x480) and the AK4493SEQ
DAC, so the Blaze port is the base. The panel itself is not the same part: the
Blaze's is an RGB565 ST77903, this board's an SLCD scanned as 32 bpp BGRA. What
follows is what had to change underneath: a different SoC revision's glibc, a
panel that wants 32 bits a pixel and one page, a codec on another ALSA card, and
a Bluetooth stack that is not the port's.

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
Blaze port uses. `sonix-player/docker/build.sh` runs `make target` in the image
`sonix-player/docker` describes, so the toolchain's own build dependencies stay
out of the host.

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

The toolchain is not in the repository (`.gitignore` keeps `rockbox-toolchain`
out: it is hundreds of megabytes), but the script that builds it is, and so are
the patches it applies. The first `sonix-player/docker/build.sh` runs it inside
the image, which is where its build dependencies (autotools, makeinfo, a native
compiler) come from:

```sh
sonix-player/docker/build.sh     # the toolchain first, then the player
```

On the host it is the same script, and `RBDEV_HOST_PREFIX` is where its build
lands — `sonix-player/rockbox-toolchain` is where the Makefile looks for it:

```sh
cd sonix-player/rockboxdev
RBDEV_HOST_PREFIX="$PWD/../rockbox-toolchain" ./rockboxdev.sh --without-bluetooth-libs
cd ..
make target
```

`--without-bluetooth-libs` is not optional in practice: without it the script
goes on to build libffi, zlib, glib, expat and dbus — the libraries of a
Bluetooth userland built from source — and dies in glib's `configure`, which
finds no gettext. This port does not link any of them: its Bluetooth userland is
prebuilt, in `sonix-packer/assets`, so `libc` and `libasound` out of the sysroot
are all it needs. The Makefile's rule for the toolchain passes the flag, so
`make target` alone does the right thing.

With the variable left alone the script installs next to itself instead, and the
tree then has to be moved or linked. `RBDEV_DOWNLOAD=<dir>` keeps the downloaded
tarballs for the next run. An existing toolchain is reused rather than rebuilt:
`make ROCKBOX_TOOLCHAIN_DIR=/path/to/rockbox-toolchain target`.

Two fetches in that build are known to be fragile on some networks, and both
can be satisfied by hand:

- **freetype** comes from `savannah.gnu.org`, which stalls on the network this
  port was built on. The Makefile skips the download when
  `sonix-player/freetype-2.13.3.tar.xz` is already there, and a sourceforge
  mirror serves the same file
  (`.../project/freetype/freetype2/2.13.3/freetype-2.13.3.tar.xz`).
- **LVGL** is cloned from github by the same Makefile; a clone that fails
  half-way leaves `sonix-player/lvgl/` behind and is best removed before
  retrying.

## Display: 32 bits, one page, a layout at two thirds

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
- LVGL draws RGB565. At 32 bpp it always draws into a **RAM page the size of
  the panel (320x480)**, and the copy rewrites it into the panel's BGRA:
  `fb_store_pixel()` widens the fields (5->8, 6->8, full scale kept full scale)
  and writes one 32-bit word per pixel, opaque. One pixel in, one out: no
  filter, no resample.
- The pages themselves are still authored at **480x720**, the R3 Pro II's and
  the Blaze's numbers, but `SC()` (`sonix-player/src/gui/shell/scale.h`) is `2` for this
  build (`SONIX_LAYOUT_SCALE`, from `BOARD` in the Makefile), so every absolute
  size and position is two thirds of what the source says and the compiler
  folds it away. The interface is laid out at 320x480; it is not laid out large
  and shrunk at run time. The older 9/3/3/1 software downscale is still in
  `main.c` (`fb_scaled`) for the boards that draw at 480x720; this board does
  not take that path (`native_panel = true` in the model).

**One framebuffer page** (`single_fb_page` in the model, `true` here). The
driver does offer two pages, but on this panel:

- with two, the frame on the glass is always the one drawn **two frames ago**
  -- the volume pill appears on the next scroll, and nothing else appears
  either;
- and each page owes the other everything that changed while it was off
  screen, so every frame pays a **full-screen copy (about 26 ms)**.

With one page and a pan, only the frame's own damage is copied: **about 2 ms**
for the same frame. The two-page path stays in `main.c` for the boards that use
it (`rot_page_pending`); this board's model takes the single page because of the
measurement above.

A blank or unblank rewrites the mode underneath the player: the driver resets the
virtual size and the descriptor when the panel powers down, and from then on the
`yoffset` that was right before is refused with `EINVAL` -- every frame, for ever.
On the device that is a lit screen showing the last frame that ever reached it
while the interface behind it keeps drawing: a frozen picture, a clock that
stopped, an indicator still saying what was true minutes ago. The pan is retried
once after re-reading the mode (`fb_mode_stale`), which is all it takes.

## Power: the backlight and the rails, then the SoC sleeps

Screen-off on this device is **two steps**, exactly as the stock player does it: the
backlight goes to 0 at `/sys/class/backlight/pwm-backlight.0/brightness`, and then the
vendor `SET_SLEEP` ioctl (`0x80044620`) powers the panel down -- `LCDC_CTRL.ENA`, the LCD
clocks, the AXP2101 rails `LDO7`/`LDO8`, the reset line and the touch controller's doze.
`blank_panel = false` in the model and the ioctl is `panel_sleep_ioctl`; `FBIOBLANK` is
never used, because the state *it* leaves behind (`ENA` back on, `DMA_EN` still down) is the
one this panel does not come back from without a reboot. The full evidence is in
[PANEL-POWER.md](PANEL-POWER.md).

What that means outside the panel, the wake-ups the SoC still serves, and the defects this
port had in this area until 2026-10-24 (a panel left powered after a wake from standby, an
`FBIOBLANK` at every boot, a frame drawn after the panel was dead, a charger that forbade
standby) are in [POWER.md](POWER.md).

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

## Sound: two cards, chosen by port, the way the stock player does it

The board has two outputs and they are two ALSA cards. Card 0 is `sa_spdif`,
the optical transmitter (the "dummy" in its name is the *codec* side of the DAI
-- S/PDIF out needs no codec; `jz_spdif_hw_params` runs and the DMA starts when
it is played to). Card 1 is `v3_analog`, the AK4493. The mixer and the analogue
output are on card 1:

- the mixer (`AK4493 Gain Control`, `Digital Filter`, `Output Port Switch`, the
  playback volumes) is opened as **`hw:1`** (`mixer_card = 1`);
- the analogue output is opened as **`hw:1,0`** (`pcm_device`);
- the optical output is opened as **`hw:0,0`** (`digital_pcm`).

`hw` and not `plughw`, because what both cards take has now been measured
(`aplay --dump-hw-params` on the device):

| | formats | channels | rates |
|---|---|---|---|
| `hw:1,0` (analogue) | `S16_LE`, `S24_LE` | 1-2 | 8000-768000 |
| `hw:0,0` (optical) | `S16_LE`, `S24_LE` | 1-2 | 8000-384000 |

`S16_LE` for a 16-bit source and `S24_LE` (24 bits in a 4-byte word) for
anything deeper is exactly what the stock player asks its tinyalsa stream for,
and it is also what keeps the alsa-lib plug out of the path: a plug with
nothing to convert passes writes straight to the driver's copy path. The plug
survives only as a fallback, taken when a card refuses a format
(`open_pcm_device()`), and the format is never silently converted otherwise.

Which card is opened is decided by the port, as in the stock player, whose
`Output Port Switch` values were read out of the binary
(`output_port_detect()`): **0** S/PDIF, **1** line out, **2** headset (3.5 mm),
**3** balanced (4.4 mm), **4** i2s, **5** none. `detect_output()` maps the
`headset`/`balance` switches onto that table; with nothing plugged in it
returns 0, which is the stock player's own answer (its port table gives
`spdif` priority 1 and its `default` port is `spdif` -- the measured file
descriptors agree: no jack, `pcmC0D0p`). The route is written from
`open_pcm_device_now()` before every open, so a jack plugged in while nothing
was playing is picked up by the next track, and the playback loop re-reads
`alsa_output_key()` every eight periods to reroute mid-track.

The optical jack's own switch, `/sys/class/switch/digital_dt2` (GPIO PB21), is
a presence input and is not consulted: the stock player does not consult it
either, and the route is 0 with and without a cable.

The stream geometry is the stock player's too, read from its device layer
(`period_count = 4`, and a period chosen by rate: 1024 frames at 8 and
11.025 kHz, 2048 up to 22.05, 4096 at 32/44.1/48, 8192 up to 192, 32768
above). At 44.1 kHz that is `buffer 16384 frames (371 ms), period 4096 frames
(92 ms)`, and the player prints those two numbers on every open. The deeper
buffer this port used to open with can be had back with `[audio] period_count`
or `[audio] buffer_time_ms`, which is what the two knobs exist for.

Both names are model fields (`sysinfo.h`), so the Blaze and the HiBy boards
keep their `hw:0` and `default` and their single-output routing.

### What actually reaches the card, measured

`tools/pcmtap.c` is a preload that dumps the frames a player hands the card, and
the answer for one 24-bit / 44.1 kHz FLAC (`17 How Far？`, Gorillaz) is that the
samples arrive **bit-identical to the file** -- no resampling, no dither, no
attenuation, the verbatim decoded stream:

| route | port | stock `hiby_player` |
|---|---|---|
| jack, 4.4 mm (`hw:1,0` / `pcmC1D0p`) | 22.2 s (7 831 552 B), identical | 17.09 s (753 664 frames), identical |
| optical, nothing plugged (`hw:0,0`) | 90.37 s (3 985 408 frames), identical | node accepts writes, content is **not audio** |

Three things that were not obvious and cost the measurements:

- **The stock player does not use alsa-lib for playback.** It links
  `libasound.so.2` and imports `snd_pcm_*` (that is its record path and the USB
  DAC), but it opens `/dev/snd/pcmC%dD%dp` itself -- through `openat64`, so a
  preload has to hook that and the `ioctl`s, not `snd_pcm_writei`. Its strings
  are explicit: `(%d)Unable to open PCM /dev/snd/pcmC%uD%u%c.`
- **With nothing plugged the stock feeds card 0 a stream that is not the
  music.** Every byte lane of that dump is uniformly distributed (mean ~128,
  sd ~127), while the same node, driven by this port, carries the track
  bit-exactly. The card's own driver name says what it is: `SA SPDIF Dummy ...
  spdif dump`. The audible path there goes through the vendor
  `/dev/sa_hgl_dma` (opened with `0xf0`/`0xe3`/`0xe2`/`0x81` ioctls), which
  takes no `write()`, no `mmap` and no per-period ioctl -- its buffer is filled
  with plain memory stores. Plugging a jack moves the stock to card 1, where it
  is ordinary and exact.
- **The frame size cannot be read from the format.** `/proc/asound` on this
  kernel has no `hw_params` (the card directory holds `id`), and the format mask
  in `struct snd_pcm_hw_params` reports the *same* bit for a 16-bit and a 24-bit
  stream. `period_bytes / period_size` (and the buffer pair) is the unambiguous
  source: 4096/32768 and 16384/131072 both say 8 bytes per frame. The struct's
  intervals are **12** bytes, not 24 -- `snd_interval` packs its flags into bit
  fields, which is why the whole thing is 604 bytes.

Volume is not in these numbers at all: on both the port and the stock the level
of the jack route lives in the AK4493, and a capture taken with the volume left
wherever it was is still the file. The port's software volume (`swvolume.c`)
only ever runs on the USB-C port.

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

The headless stand the runs were made with (headless receiver, HCI capture,
phone driven over adb) lived in `analysis/v3-bt-scripts/` in the older working
copy and is not part of this repository. `tools/btadm.c`, the D-Bus tool it
needs, is: adapters, discovery, pairing agent, connect, and a dump of
bluealsa's transports.

## The image: `v3_analog.upt`

The port is a firmware, not a folder on a card: `sonix-packer/v3_analog_packer.sh`
builds an image that boots it, taking the stock firmware as the starting point.

```
cd sonix-packer
STOCK="<the stock v3_analog.upt>" ./v3_analog_packer.sh
```

The stock image itself is not in the repository. It is the vendor's own
firmware for this model -- **1.3**, the build the port is developed against;
copies circulate on the 4pda.to forum -- and it is the one input a clone has to
bring from outside. Before starting, check it is the right one:

| in the stock `.upt` | expected |
|---|---|
| `UIMAGE.BIN` | md5 `35889275f8bed2f967a18f6a8b66495c` |
| `UBOOT.BIN` | md5 `1f54858d6b4e6f482a81a3378c0e5bf8` |
| `VERSION.TXT` | `name=v3_analog`, `ver=2024-12-24T11:00:26+08:00` |

The packer reads the bootloader out of it and carries it into our image
untouched, so it refuses a stock image whose `UBOOT.BIN` is not the one above:
a different bootloader is the one mistake on this device that cannot be undone
from a card. A vendor version newer than 1.3 has its own digest, and the check
takes it on the command line then (`STOCK_UBOOT_MD5=<md5>`).

What it does, in order: unpacks the stock `.upt` (an ISO) and extracts
`SYSTEM.UBI` (the UBIFS rootfs) from it; deletes `usr/bin/hiby_player` and
installs `sonix_player`; installs the launcher (`usr/bin/sonix_player.sh`) and
`sonix_launch` and points `etc/init.d/S92_03_start_music_player` at them; copies
`assets/BLAZE/` -- the interface assets of the Blaze port, another player whose
UI this one shares: fonts, languages, images and the Bluetooth userland -- and
then `assets/V3ANALOG/` over it; writes the build
stamp into `system-info.json`; deletes the stock interface's own resources
(`litegui`, `layout`, `str`, `fonts`); repacks the rootfs with `mkfs.ubifs`
(`-c 480 -e 0x1F000 -m 0x800`, the geometry of this device's rootfs volume),
recomputes the rootfs digest in `UPDATE.TXT`, and rebuilds the ISO with
`genisoimage`. Then it unpacks its own result and checks it: the digest, the
files, their modes, and that the Bluetooth userland in the image is the one the
assets carry and not the stock's.

### The version the player reports

`OS_version` in `usr/resource/sonix/components/system-info.json` is the row the
interface shows as **Operating system version**, and the packer writes it per
build instead of leaving it to whoever edited the asset last. The base is the
port's own -- everything in the asset's value up to the first `-` or `/`, today
`1.3.0` -- and what follows the slash names the build, out of this repository:

| the tree in this repository | the string |
|---|---|
| HEAD carries a tag | `1.3.0/0.1.0` |
| a commit, no tag | `1.3.0/#5436fdd` |
| changes not committed yet | `1.3.0/f2e2493d` |

The third row is a test build, and its eight hex are an md5 of the changes git
has not committed: the image's own md5 cannot be written inside the image, since
it is computed *from* the image. The artefact carries it in its name instead,
which is what the older images did by hand -- `out/v3_analog-ea3aded4.upt`.

A release whose commit carries no tag -- the tag lives on another branch, or the
tree has moved on since -- is named by hand, which sidesteps git rather than
moving tags around:

```
VERSION_OVERRIDE=1.3.0/0.1.0 ./v3_analog_packer.sh
```

`build_version` is a row of its own: the mtime of the player binary, the stamp
the interface shows as the build number (`151020262247`). The update check
compares release tags against `OS_version` and ignores anything after the
numbers, so a `/` there costs nothing.

`UBOOT.BIN` is carried across untouched, deliberately: writing the bootloader is
the one way to brick this device, and the update route does not need it.

One thing the verify step exists for, learned the hard way:
- **The executable bit on the init script.** The packer rewrites
  `etc/init.d/S92_03_start_music_player` in place, and a rewrite that drops its
  mode leaves a firmware whose init skips it -- busybox init does not run a
  script without the bit. The player then never starts, and what stays on the
  screen is the boot logo, which the bootloader draws from the `logo`
  partition, not the player. It looks exactly like a player that hangs. The
  rewrite preserves the mode and the check refuses an image where any of
  `S92_03_start_music_player`, `sonix_player`, `sonix_player.sh` or
  `sonix_launch` is not executable.

### ADB belongs to the interface

On this firmware adb is a USB mode of the legacy gadget, and the **Developer
options → ADB** switch is the only thing that sets it: `adb_set_enabled()` in
`sonix-player/src/system/device/adb.c` remembers the choice in the settings and
applies it through the firmware's `etc/init.d/adb/S310adb`; at boot,
`adb_apply_saved_state()` reapplies whatever was saved (off by default) and does
nothing when it is off. Nothing starts adb on its own.

The image deliberately carries no boot-time adb script. An earlier build had
`etc/init.d/S98adb_debug`, a watchdog that put the gadget into adb mode and
started `adbd` whenever neither `sonix_player` nor `sonix_launch` was running --
that is, at every boot and whenever the player was restarting -- so adb came up
despite the switch being off, and each re-arm rebuilt the USB gadget under the
running system (the card's mount point moving with it; see the duplicate-tracks
item in `TODO.md` of the original working copy, outside this repository). It is
removed, and the packer no longer expects it.

What that costs: if the player cannot start at all, adb does not come up either,
so there is nothing to look at over USB and a device stuck on the boot logo is
diagnosed with the recovery combo (Power + Prev) and the card, not with `adb
devices`. Recovery, and flashing from it, do not depend on adb.

The tools the packer runs live in a docker image this repository carries the
recipe for: `sonix-packer/docker` holds 7z, ubi_reader, mkfs.ubifs and
genisoimage, none of which a normal host has, and the base is pinned by digest
so a clone builds the same image later.

```sh
docker build -t tempotec-fw-tools sonix-packer/docker
```

### What a fresh clone needs

A clone of this branch has the packer, its assets (the Blaze port's interface,
another player this one takes its fonts and its Bluetooth userland from), the
player's sources and this document. What it does not have, and what the build
therefore asks for by name:

| input | how to get it |
|---|---|
| docker | the host's own; the scripts refuse to start without it |
| the tools image | `docker build -t tempotec-fw-tools sonix-packer/docker` |
| the player's image | `docker build -t tempotec-player-build sonix-player/docker`, or let `sonix-player/docker/build.sh` build it |
| the stock `.upt` | the vendor's 1.3 firmware for the V3 Analog, above; not in the repository |
| `sonix-packer/sonix_player`, `sonix_launch` | `sonix-player/docker/build.sh` (see "Building for the target") |
| `sonix-packer/kernel/UIMAGE.BIN` | nothing: it must **not** be there. The packer installs a kernel from that slot when one is present, and only kernel work deliberately puts one there (`patch/v3-kernel/FLASHING.md` in the kernel working copy next to this repository; the mechanism is in `docs/KERNEL.md`) |

The packer checks each of these before it starts and says which one is missing,
so a first run on a fresh clone is a list of what to fetch, not a stack of
errors from `mkfs.ubifs`.

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
name and treated anything starting with `plughw:` as the USB port. At the time
the analogue output *was* opened as `plughw:1,0` (it is `hw:1,0` now), so the
profile was always "USB". Its ceiling was then the only one ever enforced
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

## Upstream base: 1.3.0

The player's source is a fork of [Jepl4r/sonix-player](https://github.com/Jepl4r/sonix-player):
the tree carries that project's history, so an update is a merge of its release
tag, not a patch-by-patch port. The base this image was built on is **1.3.0**
(`a488e25`, 16 October 2026); the tree it was taken from, `d9a0f1b` (10 October),
is what the two ports share as an ancestor. The recipe for taking the next one
-- the merge, the interface scaling, the icons, the language files -- is in
[UPDATING.md](UPDATING.md).

What is deliberately **not** taken from upstream, because this board has no path
for it:

- the kernel and its modules (`axp2101.ko`, `soc_fb.ko`, `brcmfmac.ko`, zram):
  the V3 Analog keeps TempoTec's 3.10 kernel;
- the boot logo by theme (`/etc/boot-logo`, `S11jpeg_display_shell`):
  `boot_logo_flash = false` and `one_boot_logo = false` in `board.h`, because
  `/dev/mtd5` here is the `/data` UBI volume, not a scratch partition. The
  kernel shows TempoTec's own logo before the player exists;
- the charging screen while off: it reads its marker from the AXP2101 over I2C,
  and there is no `/dev/i2c-0` on this board, so `chargescreen_run_if_wanted()`
  returns at once;
- the charge limit: same reason, see [PANEL-POWER.md](PANEL-POWER.md) and the
  list below.

## What does not work, and what is not verified

Not working, with the reason:

- **Charge limit** (*fixed 2026-10-24*): implemented through the AXP2101's own
  charge current, `/sys/class/power_supply/usb/constant_charge_current_max`
  (REG62h) -- 0 holds the battery, the previous value lets it go. The PMIC's
  `charge_control_limit_max` is not used: writing 100 to it also drops the
  input current limit to 100 mA and starves the system. There is still no
  `/dev/i2c*` on this firmware, so anything else the PMIC can do is reachable
  only through the driver's raw node, `/sys/class/axp/axp_reg` (read a register,
  or write `0xRRVV`). See [POWER.md](POWER.md) for the register evidence, and
  for the reason the cable keeps the device awake.
- **Optical / S/PDIF at rates above 192 kHz**: the route and the card are
  implemented (see "Sound" above: nothing plugged in means `hw:0,0` and
  `Output Port Switch` 0, as in the stock player), and the card itself accepts
  up to 384 kHz. The stock player limits its digital port to 44.1-192 kHz and
  resamples above that; the port does not resample, so a 352.8/384 kHz DoP
  track would go out at a rate no S/PDIF receiver is specified for. What an
  actual receiver does with it has not been measured.
- **Smoothness of page opens**: the first frame of a new page takes 96-135 ms
  (widget building, font rasterisation, the blurred backdrop); scrolling is at
  about 45 fps. Profiling and the cheap wins are still to be done.

Verified on the device:

- the physical keys, including `media_keys_swapped = true` (the kernel calls the
  upper skip key `KEY_NEXTSONG`, so it has to be read as "back", as on the
  R3 Pro II);
- sound by ear: wired 3.5 mm and 4.4 mm, Bluetooth out (A2DP), Bluetooth in
  (receiver), and optical;
- screen-off and wake through the panel's own `SET_SLEEP` ioctl, with the pan fix
  above (the rails go down and come back; see
  [PANEL-POWER.md](PANEL-POWER.md#9-on-the-device-what-was-measured-and-what-the-port-now-does));
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
