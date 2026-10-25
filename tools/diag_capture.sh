#!/bin/sh
# Dumps everything worth having while the device is still standing, and does it
# in one go so a device that is fading can be read before it goes.
#
# Run it the moment the player stops answering, before touching the power
# button:
#
#     adb shell /data/diag_capture.sh
#
# If adb answers, the SoC is alive and this tells us where it is stuck: the
# state of every thread of the player, what the blocking kernel threads are,
# the card driver's counters, and the virtual memory figures the sample log only
# guessed at. Everything lands in /data/diag/capture-<time>.txt.
STAMP=$(date '+%Y%m%d-%H%M%S')
OUT=/data/diag/capture-$STAMP.txt
mkdir -p /data/diag

{
	echo "capture $STAMP"
	echo "date: $(date)  uptime: $(cat /proc/uptime)"
	echo
	echo "=== meminfo"; cat /proc/meminfo
	echo; echo "=== vmstat"; cat /proc/vmstat
	echo; echo "=== loadavg"; cat /proc/loadavg
	echo; echo "=== mmcblk0 stat"; cat /sys/block/mmcblk0/stat
	echo; echo "=== interrupts"; grep -E 'mmc|i2c|jz-fb' /proc/interrupts
	echo; echo "=== mounts"; grep sd_0 /proc/mounts
	echo; echo "=== top (by state)"; ps
	echo; echo "=== all thread states"
	for t in /proc/[0-9]*; do
		[ -r "$t/status" ] || continue
		st=$(awk '/^State:/{print $2}' "$t/status" 2>/dev/null)
		case "$st" in
		D|R) echo "pid=$(basename "$t") comm=$(cat "$t/comm") state=$st wchan=$(cat "$t/wchan" 2>/dev/null)" ;;
		esac
	done
	PID=$(pidof sonix_player | cut -d' ' -f1)
	if [ -n "$PID" ]; then
		echo; echo "=== player $PID threads"
		for t in /proc/"$PID"/task/*; do
			[ -r "$t/status" ] || continue
			echo "tid=$(basename "$t") comm=$(cat "$t/comm") state=$(awk '/^State:/{print $2}' "$t/status") wchan=$(cat "$t/wchan" 2>/dev/null)"
			grep -E '^(voluntary|nonvoluntary)' "$t/status" 2>/dev/null | tr '\n' ' '; echo
			cat "$t/stack" 2>/dev/null | head -8
		done
		echo; echo "=== player status"; grep -E 'Vm|Threads|Sig' /proc/"$PID"/status
	fi
	echo; echo "=== dmesg tail"; dmesg | tail -120
} >"$OUT" 2>&1

# The kernel's own view of every blocked task, into the ring buffer: it is the
# only thing that reads a stack that userspace cannot see.
echo w >/proc/sysrq-trigger 2>/dev/null && sleep 1
dmesg | tail -200 >>"$OUT" 2>&1

# Bounded on purpose: on a device that is hanging, sync() is exactly the call
# that blocks forever, and the file above is worth more than a tidy cache.
timeout 10 sync 2>/dev/null
echo "written: $OUT"
