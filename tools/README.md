# tools

Small cross-compiled programs written while bringing the port up on the V3
Analog. None of them is part of the player; each answers one question the
device could not answer from the player's log, and each is meant to be pushed
to the card and run there.

All of them are built with the same MIPS toolchain as the player
(`../sonix-player/rockbox-toolchain/bin/mipsel-rockbox-linux-gnu-*`, which is where the
player's own build puts it; see [../docs/V3-ANALOG.md](../docs/V3-ANALOG.md)).

## panctl

Reads `/dev/fb0`'s own idea of where it is looking and moves it.

```
panctl            print the mode and the current yoffset
panctl <yoffset>  pan there and say so
```

Written for the SLCD panel, whose frame reaches the glass only through
`FBIOPAN_DISPLAY`. Everything about the display path -- which page is on
screen, whether the driver honours a pan, whether the panel shows the page a
frame was just written into -- is answered by putting two known pages in
memory and looking at the glass.

```
mipsel-rockbox-linux-gnu-gcc -static -O2 -o panctl panctl.c
```

Static on purpose: it has to run on a device whose glibc is older than the
toolchain's, and a static binary asks the loader for nothing.

## sleepfb

The stock player's panel sleep, measured: the proprietary `SET_SLEEP` ioctl
(`0x80044620`) that takes the panel rails, reset and touch controller down
without suspending the SoC (see [../docs/PANEL-POWER.md](../docs/PANEL-POWER.md)).

```
sleepfb status        registers and rails as they are
sleepfb off           SET_SLEEP(1), then print what changed
sleepfb on            SET_SLEEP(0), the driver pans a frame back
sleepfb cycle 5       off, wait 5 s, on
```

It prints `LCDC_CTRL.ENA`, `SLCDC_CTRL.DMA_EN/DMA_START`,
`SLCDC_STATE.BUSY`, the `lcd_3v3`/`lcd_1v8` rails and the backlight level
around the call. The backlight is left alone on purpose: on the stock player
it is a separate write, and a lit black glass is the expected intermediate
state.

```
mipsel-rockbox-linux-gnu-gcc -static -O2 -o sleepfb sleepfb.c
```

Run it from the card while the player is running; it needs `/dev/mem` for the
registers and `/dev/fb0` for the ioctl, nothing else.

## btprobe

What the system bus says about Bluetooth, without `bluetoothctl` (which is not
on the device and needs readline).

```
btprobe        adapters, their state, and whether bluealsa is on the bus
btprobe -d     the same, plus every device the adapter knows
```

It is one `GetManagedObjects` call on `org.bluez` -- the BlueZ 5
ObjectManager layout (`/org/bluez/hci0`, `/org/bluez/hci0/dev_XX_...`) -- plus
a `NameHasOwner` for `org.bluez` and `org.bluealsa`. That is how a BlueZ 5
`bluetoothd` put in place of the stock 4.101 is confirmed to have come up and
found the controller. Built against the device's own `libdbus-1`:

```
mipsel-rockbox-linux-gnu-gcc -O2 -o btprobe btprobe.c \
    -I<sysroot>/usr/include/dbus-1.0 \
    -I<sysroot>/usr/lib/mipsel-linux-gnu/dbus-1.0/include \
    -L<sysroot>/usr/lib -ldbus-1
```

## tprobe

What the kernel reports from an input device, and nothing else.

```
tprobe [device] [seconds]     default: /dev/input/event2, 10 s
```

Written for the touch panel (GT9XX-TS). The question it answers cannot be
answered from the player's log: when the screen has been blanked once and the
interface then ignores touches, is the kernel still reporting them? No events
at all while a finger is on the glass means the controller itself is not
reporting -- a different bug, with a different fix, from the interface not
reading a device that is.

```
mipsel-rockbox-linux-gnu-gcc -static -O2 -o tprobe tprobe.c
```

## ldacdec-test

Is the firmware's own `libldacBT.so` decode path usable from this build?

```
ldacdec-test [channel_mode] [sampling_freq] [file-of-a-frame]
```

It takes a handle, initializes the decoder and feeds it a frame -- either 0x5a
nonsense (a working decoder answers with an error code) or a file, which is how
the two LDAC problems were told apart: linked against the combined library only,
a captured frame decodes (`used 220, decoded 1024`); linked against AOSP's
`libldacBT_enc`/`libldacBT_abr` as well, the same frame segfaults. The payloads
come from a capture with `btsnoop-payloads.py` (in `analysis/v3-bt-scripts/` of the
older working copy; not part of this repository).

```
mipsel-rockbox-linux-gnu-gcc -O2 -o ldacdec-test ldacdec-test.c \
    -I<sysroot>/usr/include/ldac -L<sysroot>/usr/lib -lldacBT \
    -Wl,--allow-shlib-undefined
```

## btadm

BlueZ 5 over D-Bus, which is what bluetoothctl would do if the device had it
(it does not: no readline, and the firmware's own `bluetoothd` is BlueZ 4).

```
btadm adapters | devices            what the adapter knows
btadm discover <seconds>            start discovery, name what turns up
btadm set <property> <bool>         powered, discoverable, pairable, ...
btadm pair|connect|disconnect <MAC> Device1 methods
btadm trust <MAC> yes|no            Device1.Trusted
btadm remove <MAC>                  Adapter1.RemoveDevice
btadm wait <MAC> <seconds>          watch one device's state
btadm agent [capability]            answer pairing requests (default NoInputNoOutput)
btadm players | player <MAC> play   the phone's player, over AVRCP
btadm pcm <MAC>                     bluealsa's transports, with codec and rate
```

`agent` is what makes a headless receiver work: with the player stopped nothing
would answer a pairing request. `player <MAC> play` starts the phone playing --
which is also how a drop gets recovered, since the phone pauses when the link
goes. `pcm` dumps the transport's properties (codec, sampling, delay) from
bluealsa, which is how "did the phone actually send anything" is answered
without a sniffer.

```
mipsel-rockbox-linux-gnu-gcc -O2 -o btadm btadm.c \
    -I<sysroot>/usr/include/dbus-1.0 -I<sysroot>/usr/lib/dbus-1.0/include \
    -L<sysroot>/usr/lib -ldbus-1
```

## powsample

One line of power state every N seconds, for a discharge measurement: battery
current and voltage, level, backlight, **the two panel rails** (`LDO7`/`LDO8`,
the panel's 3.0 V and 1.8 V), the `slcd_vsync` interrupt count and the player's
own CPU time. Written for the audit in
[../docs/POWER.md](../docs/POWER.md), whose three states -- screen on, screen
off awake, standby -- are indistinguishable from the backlight alone.

```
mipsel-rockbox-linux-gnu-gcc -static -O2 -march=mips32r2 -mno-madd4 \
    -o powsample powsample.c
adb push powsample /data/powsample
adb shell 'nohup /data/powsample /data/powsample.log 20 >/dev/null 2>&1 &'
```

It logs to `/data`, not to the card, so the log is readable over adb as soon as
the cable is back; a suspend shows up as a jump in `boot - mono`.

## Also here, undocumented

`vsynctest.c` (the panel's frame boundary), `blit_bench.c` (the RGB565-to-BGRA
copy), `panwatch.c` (one transfer, sampled from `/dev/mem`), `fbtool.c`
(registers, put, pan, poll), `hciwatch.c` (an HCI trace), `rxprobe.c` (PCM
throughput of a Bluetooth receiver), `keyinj.c` (drives the buttons from adb),
`watch_wake.sh` and `profreport.py` (`.local/profile.out` to something
readable). The icon rasteriser is the one exception: its recipe lives in
`../sonix-player/docker/icons.Dockerfile`, not here.

## The patches

- `bluez-5.66-a2dp-sink-avdtp12.patch` -- the sink's SDP record advertises
  A2DP/AVDTP 1.2 like the stock firmware's, so a phone asks for capabilities
  rather than "all capabilities" (which would drag delay reporting in).
- `bluez-5.66-avdtp-transport-mtu.patch` -- an AVDTP channel announces a receive
  MTU. Without it the media channel carries no MTU option, a phone falls back to
  the L2CAP default (672) and its LDAC encoder refuses the 660 that leaves
  (LDAC needs 679 and up), so no LDAC audio is ever sent.

## glibc216_shim.c

Not a program: two symbol definitions compiled into the Bluetooth userland
(BlueZ 5.66's `bluetoothd` and bluez-alsa's ALSA plugin) so their binaries can
start on the device's glibc 2.16. It defines `getrandom` (the syscall arrived
in Linux 3.17, this device runs 3.10, so `/dev/urandom` is the way in) and
`clock_gettime` (librt's `GLIBC_2.2` copy, where 2.16 keeps it). See
[../docs/V3-ANALOG.md](../docs/V3-ANALOG.md) for the full recipe.
