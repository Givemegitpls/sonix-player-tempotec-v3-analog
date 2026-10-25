#!/bin/sh
# Rotates the logs of the run that has just ended and starts the probe.
#
# Called once at boot, before the player is started. Everything here lives on
# /data: a hard reset of the SoC leaves the page cache on the card unwritten,
# and the whole point is to have the last seconds of a hang on disk.
#
# The kernel ring buffer needs no follower of its own: the player copies every
# message of level 4 and worse into its log, and the player log is on /data too
# (SONIX_LOG, set by the boot script that calls this one).
D=/data/diag
mkdir -p "$D"

# Only with the player down: run by hand while it plays, the move would take
# the live log out of the namespace and the player would keep writing into a
# deleted inode for the rest of the boot.
if [ -z "$(pidof sonix_player)" ]; then
	[ -s "$D/probe.log" ] && mv -f "$D/probe.log" "$D/probe.prev.log"
	[ -s /data/sonix_player.log ] && mv -f /data/sonix_player.log /data/sonix_player.prev.log
fi

# Keep the total bounded: /data has ~25 MB free and the probe is ~1 MB/day.
[ -s "$D/probe.prev.log" ] && tail -c 400000 "$D/probe.prev.log" >"$D/probe.prev.tmp" &&
	mv -f "$D/probe.prev.tmp" "$D/probe.prev.log"
[ -s /data/sonix_player.prev.log ] && tail -c 400000 /data/sonix_player.prev.log >/data/sonix_player.prev.tmp &&
	mv -f /data/sonix_player.prev.tmp /data/sonix_player.prev.log

[ -x /data/diag_probe.sh ] && setsid /data/diag_probe.sh >/dev/null 2>&1 </dev/null &
exit 0
