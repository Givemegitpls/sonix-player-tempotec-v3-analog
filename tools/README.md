# tools

Small cross-compiled programs written while bringing the port up on the V3
Analog. None of them is part of the player; each answers one question the
device could not answer from the player's log, and each is meant to be pushed
to the card and run there.

All of them are built with the same MIPS toolchain as the player
(`rockbox-toolchain/bin/mipsel-rockbox-linux-gnu-*`, see the build notes in
[../docs/V3-ANALOG.md](../docs/V3-ANALOG.md)).

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

## glibc216_shim.c

Not a program: two symbol definitions compiled into the Bluetooth userland
(BlueZ 5.66's `bluetoothd` and bluez-alsa's ALSA plugin) so their binaries can
start on the device's glibc 2.16. It defines `getrandom` (the syscall arrived
in Linux 3.17, this device runs 3.10, so `/dev/urandom` is the way in) and
`clock_gettime` (librt's `GLIBC_2.2` copy, where 2.16 keeps it). See
[../docs/V3-ANALOG.md](../docs/V3-ANALOG.md) for the full recipe.
