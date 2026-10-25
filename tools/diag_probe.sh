#!/bin/sh
# One sample a second of everything that would explain a hard hang of the whole
# SoC: free memory, direct reclaim, the SD card's own counters, the load, and
# once a minute where the threads are.
#
# /data is ubifs on the NAND: it survives a hard reset, unlike a page cache on
# the card. Every sample is synced, so the last line in the file is the state a
# second before the death.
#
# The file keeps itself within MAX bytes by dropping the older part, so a
# device left alone for days does not fill /data.
LOG=/data/diag/probe.log
MAX=4194304
KEEP=1048576
mkdir -p /data/diag

# One writer only: a second copy would interleave its samples with this one.
# The pid alone is not enough -- after a reboot the number belongs to something
# else, and the check would keep the probe off for the whole boot; the command
# line is what says it is really another probe.
if [ -r /data/diag/probe.pid ]; then
	old=$(cat /data/diag/probe.pid 2>/dev/null)
	if [ -n "$old" ] && tr '\0' ' ' </proc/"$old"/cmdline 2>/dev/null | grep -q diag_probe.sh; then
		exit 0
	fi
fi
echo $$ >/data/diag/probe.pid

exec >>"$LOG" 2>&1

# Read again every minute, not once here: the probe is started by the boot
# script *before* the player, so at this point there is no pid to find yet --
# and the whole thread dump below would then never run.
PLAYER_PID=$(pidof sonix_player | cut -d' ' -f1)
echo "--- probe started $(date '+%Y-%m-%d %H:%M:%S') uptime=$(cut -d' ' -f1 /proc/uptime) player=${PLAYER_PID:-none}"

n=0
while :; do
	n=$((n + 1))
	up=$(cut -d' ' -f1 /proc/uptime)
	free=$(awk '/^MemFree:/{print $2}' /proc/meminfo)
	cached=$(awk '/^Cached:/{print $2}' /proc/meminfo)
	slab=$(awk '/^Slab:/{print $2}' /proc/meminfo)
	dirty=$(awk '/^Dirty:/{print $2}' /proc/meminfo)
	alloc=$(awk '/^allocstall/{s+=$2} END{print s+0}' /proc/vmstat)
	mmc=$(cat /sys/block/mmcblk0/stat)
	load=$(cut -d' ' -f1-3 /proc/loadavg)
	echo "$up mem=${free}k cache=${cached}k slab=${slab}k dirty=${dirty}k stall=$alloc load=$load mmc=[$mmc]"

	# Once a minute: the threads, so a hang shows which of them was in the
	# kernel, in which state, and what the card's interrupt counters were doing
	# just before it stopped. The player's pid is looked up here and not once at
	# the start: the probe runs from boot, before there is a player to look at,
	# and the player is restarted by the launcher when it exits.
	if [ $((n % 60)) -eq 0 ]; then
		PLAYER_PID=$(pidof sonix_player | cut -d' ' -f1)
	fi
	if [ $((n % 60)) -eq 0 ] && [ -n "$PLAYER_PID" ] && [ -d /proc/"$PLAYER_PID" ]; then
		for t in /proc/"$PLAYER_PID"/task/*; do
			[ -r "$t/status" ] || continue
			comm=$(cat "$t/comm" 2>/dev/null)
			state=$(awk '/^State:/{print $2}' "$t/status" 2>/dev/null)
			echo "  tid=$(basename "$t") ${comm} state=$state wchan=$(cat "$t/wchan" 2>/dev/null)"
		done
		echo "  irq: $(grep mmc /proc/interrupts | tr -s ' ' | tr '\n' '|')"
	fi

	# Once a minute, after the file has grown: keep the recent part.
	if [ $((n % 60)) -eq 0 ]; then
		size=$(wc -c <"$LOG" 2>/dev/null)
		if [ "${size:-0}" -gt $MAX ]; then
			tail -c $KEEP "$LOG" >"$LOG.tmp" 2>/dev/null && cat "$LOG.tmp" >"$LOG" && rm -f "$LOG.tmp"
		fi
	fi

	# No sync() anywhere in here. A sample is a few hundred bytes on ubifs,
	# and ubifs commits on its own every few seconds; a periodic sync dragged
	# every dirty page in the system -- the library database's pages on the
	# card included -- out to the SD card at the probe's own rhythm, which put
	# the card at 78% busy, produced jzmmc 3-second timeouts and "database is
	# locked" in the player. The measurement must not be the load.
	sleep 1
done
