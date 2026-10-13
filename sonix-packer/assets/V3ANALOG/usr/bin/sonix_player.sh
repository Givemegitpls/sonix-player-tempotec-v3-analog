#!/bin/sh
#
# The player, and the process that outlives it.
#
# The stock launcher did the same job with its own shell: run the player, sleep
# a second, reboot. sonix_launch is those three steps without the shell, and it
# reboots even when the player cannot be started at all -- a failed exec takes
# the shell with it, and the device would sit on the last frame with nothing
# running, which reads as a hang.
#
# The stack limit and the arena count are the port's own, not decoration: LVGL
# draws on the stack, and one malloc arena is the right number for a player this
# small on a device with 56 MB of RAM.
ulimit -s 1024
export MALLOC_ARENA_MAX=1

killall    sonix_player    &>/dev/null
killall -9 sonix_player    &>/dev/null

if [ -f "/usr/bin/batd" ]; then
killall    batd    &>/dev/null
killall -9 batd    &>/dev/null
# The stock firmware ran batd with its log on the card (-o /mnt/sd_0/batlog.txt),
# where it wrote every five seconds. Nothing reads that log here.
fi

# SONIX_LOG is where the player writes its own log: the stock launcher let the
# player's output fall on the console, which nothing on this device reads back.
SONIX_LOG=/mnt/sd_0/sonix.log exec /usr/bin/sonix_launch /usr/bin/sonix_player
