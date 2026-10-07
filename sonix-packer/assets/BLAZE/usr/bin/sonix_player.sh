#!/bin/sh

ulimit -s 1024
export MALLOC_ARENA_MAX=1

killall    sonix_player    &>/dev/null
killall -9 sonix_player    &>/dev/null

if [ -f "/usr/bin/batd" ]; then
killall    batd    &>/dev/null
killall -9 batd    &>/dev/null
#/usr/bin/batd -v -s -t5 -o /mnt/sd_0/batlog.txt &
fi

#/usr/bin/sonix_player &>/dev/null

# Crash-loop guard (V3 Blaze): the launcher reboots whenever the player exits,
# so a player that dies at start would reboot the device forever. Every start
# counts here and a player still running after 60 s clears the count; after
# three short runs in a row the player is not started at all and the system
# stays up with adb, to put a known-good v3_analog_2025.upt on the card and run
# "/usr/bin/bootmode.sh Recovery; reboot". The count is dropped then, so the
# next reboot tries the player again.
FAILS=/usr/data/sonix_bootfail
n=$(cat "$FAILS" 2>/dev/null)
case "$n" in ''|*[!0-9]*) n=0 ;; esac
if [ "$n" -ge 3 ]; then
	echo "sonix_player.sh: $n failed starts in a row, player not started (adb is up)" > /dev/kmsg
	# The player is what mounts the card, so it is mounted here instead.
	sd=$(readlink -f /mnt/sd_0)
	if ! grep -q " $sd " /proc/mounts; then
		for dev in /dev/mmcblk0p1 /dev/mmcblk1p1 /dev/mmcblk0 /dev/mmcblk1; do
			[ -b "$dev" ] && mount "$dev" /mnt/sd_0 2>/dev/null && break
		done
	fi
	rm -f "$FAILS"
	sync
	exit 0
fi
echo $((n + 1)) > "$FAILS"
sync
( sleep 60; rm -f "$FAILS"; sync ) &

/usr/bin/sonix_player
sleep 1
reboot
