#!/bin/sh
# Walk the host simulator through the gestures given as arguments, taking one
# screenshot after each and printing its path.
#
#   scripts/walk.sh "160 279" "drag 160 8 160 320"
#
# The arguments are the same lines the port reads from /tmp/sonix/tap: "x y" for
# a tap, "drag x1 y1 x2 y2" for a swipe, in panel pixels (320x480 on the V3
# Analog; with PANEL=480x720 the window is the 480x720 layout instead).
#
# PANEL, SD and SP override the window size, the card root and the player tree.
# SD must be writable: without a card root of its own the port picks /mnt/sd_0,
# cannot create it, and screenshots fail.
#
# Timing: the greeting splash runs for about ten seconds on a first boot, so the
# first screenshot is taken late on purpose; a gesture needs a few seconds to be
# picked up (the port polls /tmp/sonix/tap from a timer and plays the gesture
# out over its own input reads).

set -u

HERE=$(cd "$(dirname "$0")" && pwd)
PROJECT=$(cd "$HERE/../../../.." && pwd)

SP=${SP:-$PROJECT/sonix-player}
SD=${SD:-/tmp/opencode/sd}
PANEL=${PANEL:-320x480}
LOG=${LOG:-/tmp/opencode/host.log}

mkdir -p /tmp/sonix "$SD"

cd "$SP" || exit 1
SONIX_PANEL="$PANEL" SONIX_SD_ROOT="$SD" ./sonix_player_host >"$LOG" 2>&1 &
PID=$!
sleep 12

LAST=""
shot() {
	: > /tmp/sonix/shot
	i=0
	while [ "$i" -lt 40 ]; do
		new=$(ls -t "$SD/Screenshots"/*.png 2>/dev/null | head -1)
		if [ -n "$new" ] && [ "$new" != "$LAST" ]; then
			LAST="$new"
			echo "$new"
			return
		fi
		sleep 1
		i=$((i + 1))
	done
	echo "(no screenshot)"
}

echo "boot: $(shot)"
n=0
for gesture in "$@"; do
	n=$((n + 1))
	printf '%s\n' "$gesture" > /tmp/sonix/tap
	sleep 5
	echo "gesture$n '$gesture': $(shot)"
done

kill "$PID" 2>/dev/null
wait "$PID" 2>/dev/null
