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

## What does not work, and what is not verified

Not working, with the reason:

- **USB-DAC** (the player as a sound card for a computer): the kernel does not
  offer the UAC function. Creating the configfs gadget node fails with
  `Function not implemented`, there is no `/dev/uac*`, and the kernel has no
  `u_audio`/`usb_f_uac*` modules (all drivers are built in, none are shipped).
  This needs a different kernel -- a project of its own.
- **Charge limit**: the AXP2101 is not reachable over I2C here (`/dev/i2c-0`
  does not exist); the stock firmware goes through
  `/sys/class/axppower` and `/sys/class/power_supply/usb` instead. Until that
  is written, the switch is off rather than pretending.
- **Serial number in About**: the device's node is
  `/proc/jz/efuse/efuse_serial_nb`, not the `efuse_chip_id` the port reads.
- **Optical / S/PDIF**: the route to card 0 (`sa_spdif`) on the `digital_dt2`
  switch is not implemented.
- **LDAC receive**: newer AOSP `libldac` ships the encoder and ABR only; the
  decoder is gone, so LDAC can be sent but not received.
- **A flashable image**: this repository carries the source changes, not a
  packed `.upt`.

Not yet verified on the device (owed a test):

- the physical keys (volume and skip) and `media_keys_swapped` -- the model
  keeps the stock ordering until the case is checked;
- sound by ear on the `plughw:1,0` route and the `headset`/`balance` switches;
- the exact formats and rates `plughw:1,0` accepts;
- screen-off and wake through the framebuffer blank node.
