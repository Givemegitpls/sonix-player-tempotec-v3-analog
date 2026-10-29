#!/bin/sh
# Blink the backlight from userspace, to prove the register sequence our kernel's bootblink
# uses is the right one (GPIO 89 = port C bit 25 = "BL PWR" in the vendor's dmesg).
#
# Port C's registers are at physical 0x10010200 + offset (GPIO_IOBASE 0x10010000, port
# stride 0x100):
#   PXINT 0x10, PXMSK 0x20, PXPAT1 0x30, PXPAT0 0x40, PXOENC 0x68
# A pin's function is the four bits (PXINT, PXMSK, PXPAT1, PXPAT0); "output high" is
# PXINT=0, PXMSK=1, PXPAT1=0, PXPAT0=1, and the level is PXPAT0.
#
# Read-modify-write because devmem cannot: every register is read first, the original values
# are put back at the end.

P=0x10010200
BIT=0x02000000

o_int=$(devmem $((P + 0x10)) 32)
o_msk=$(devmem $((P + 0x20)) 32)
o_p1=$(devmem $((P + 0x30)) 32)
o_p0=$(devmem $((P + 0x40)) 32)
echo "before: PXINT=$o_int PXMSK=$o_msk PXPAT1=$o_p1 PXPAT0=$o_p0"

# function = output, level high
devmem $((P + 0x10)) 32 $((o_int & ~BIT))
devmem $((P + 0x20)) 32 $((o_msk | BIT))
devmem $((P + 0x30)) 32 $((o_p1 & ~BIT))
devmem $((P + 0x40)) 32 $((o_p0 | BIT))
# enable the output driver
devmem $((P + 0x68)) 32 $BIT
devmem $((P + 0x74)) 32 $BIT

echo "blinking"
i=0
while [ $i -lt 3 ]; do
	devmem $((P + 0x40)) 32 $((o_p0 | BIT))
	sleep 1
	devmem $((P + 0x40)) 32 $((o_p0 & ~BIT))
	sleep 1
	i=$((i + 1))
done

# put it back
devmem $((P + 0x10)) 32 $o_int
devmem $((P + 0x20)) 32 $o_msk
devmem $((P + 0x30)) 32 $o_p1
devmem $((P + 0x40)) 32 $o_p0
echo "restored: PXINT=$(devmem $((P + 0x10)) 32) PXMSK=$(devmem $((P + 0x20)) 32) PXPAT1=$(devmem $((P + 0x30)) 32) PXPAT0=$(devmem $((P + 0x40)) 32)"
