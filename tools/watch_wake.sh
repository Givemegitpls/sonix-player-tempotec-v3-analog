#!/bin/sh
# Passive capture of a panel wake: nothing here pans or blanks the panel.
# Samples the LCDC/SLCDC registers (via devmem, read-only), the fb nodes and
# the display IRQ counters, and follows dmesg, for the whole wake window.
OUT=/data/wake-$(date +%H%M%S).log
{
	echo "=== start $(date) uptime $(cut -d' ' -f1 /proc/uptime)"
	echo "=== fb nodes before"
	for n in mode state pan blank virtual_size stride bits_per_pixel; do
		printf '%s: ' "$n"; cat "/sys/class/graphics/fb0/$n" 2>&1; echo
	done
	echo "=== interrupts before"
	grep -iE "lcd|slcd" /proc/interrupts
	echo "=== registers before"
	printf 'LCDC_CTRL(30)=%s LCDC_STATE(34)=%s SLCDC_CTRL(a4)=%s SLCDC_STATE(a8)=%s LCDC_CFG(00)=%s\n' \
		"$(devmem 0x13050030 32)" "$(devmem 0x13050034 32)" "$(devmem 0x130500a4 32)" \
		"$(devmem 0x130500a8 32)" "$(devmem 0x13050000 32)"
} > "$OUT" 2>&1

# Follow the kernel log in the background (the lines the driver prints per pan).
dmesg -w >> "$OUT" 2>&1 &
DMESG=$!

# Sample loop. No sleep: the devmem/awk subprocesses pace it at a few Hz.
i=0
while [ $i -lt 4000 ]; do
	printf 'T=%s LCDC_CTRL=%s ENA=%s LCDC_STATE=%s SLCDC_CTRL=%s DMA_EN=%s SLCDC_STATE=%s BUSY=%s lcdc=%s vsync=%s fbstate=%s pan=%s blank=%s\n' \
		"$(cut -d' ' -f1 /proc/uptime)" \
		"$(devmem 0x13050030 32)" "$(( ($(devmem 0x13050030 32) >> 3) & 1 ))" \
		"$(devmem 0x13050034 32)" \
		"$(devmem 0x130500a4 32)" "$(( $(devmem 0x130500a4 32) & 1 ))" \
		"$(devmem 0x130500a8 32)" "$(( $(devmem 0x130500a8 32) & 1 ))" \
		"$(grep -m1 'lcdc-1' /proc/interrupts | awk '{print $2}')" \
		"$(grep -m1 'slcd_vsync' /proc/interrupts | awk '{print $2}')" \
		"$(cat /sys/class/graphics/fb0/state 2>/dev/null)" \
		"$(cat /sys/class/graphics/fb0/pan 2>/dev/null)" \
		"$(cat /sys/class/graphics/fb0/blank 2>/dev/null)" >> "$OUT"
	i=$((i + 1))
done

kill $DMESG 2>/dev/null
{
	echo "=== fb nodes after"
	for n in mode state pan blank virtual_size stride bits_per_pixel; do
		printf '%s: ' "$n"; cat "/sys/class/graphics/fb0/$n" 2>&1; echo
	done
	echo "=== interrupts after"
	grep -iE "lcd|slcd" /proc/interrupts
	echo "=== end $(date) uptime $(cut -d' ' -f1 /proc/uptime)"
} >> "$OUT" 2>&1
echo "captured: $OUT"
