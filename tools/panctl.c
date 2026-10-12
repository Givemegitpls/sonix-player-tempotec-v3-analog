// panctl -- read the framebuffer's own idea of where it is looking, and move it.
//
// Written for the TempoTec V3 Analog, whose panel is an SLCD: a frame reaches
// the glass only when FBIOPAN_DISPLAY hands the descriptor to the controller
// and restarts its DMA. Everything about the double-buffered display path --
// which page is on screen, whether the driver honours a pan at all, whether the
// panel shows the page a frame was just written into -- is answered by looking
// at the glass with two known pages in memory, which is what this is for.
//
//   panctl            print the mode and the current yoffset
//   panctl <yoffset>  pan there and say so
//
// Static on purpose: it is a diagnostic that has to run on a device whose glibc
// is older than the toolchain's, and a static binary does not ask the loader for
// anything.
//
// Build (from sonix-player):
//   rockbox-toolchain/bin/mipsel-rockbox-linux-gnu-gcc -static -O2 -o panctl panctl.c

#include <fcntl.h>
#include <linux/fb.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>

int main(int argc, char **argv) {
	int fd = open("/dev/fb0", O_RDWR);
	if (fd < 0) {
		perror("/dev/fb0");
		return 1;
	}

	struct fb_var_screeninfo var;
	if (ioctl(fd, FBIOGET_VSCREENINFO, &var) < 0) {
		perror("FBIOGET_VSCREENINFO");
		return 1;
	}

	if (argc < 2) {
		printf("xres %u yres %u | virtual %u x %u | %u bpp | yoffset %u\n", var.xres, var.yres, var.xres_virtual,
			   var.yres_virtual, var.bits_per_pixel, var.yoffset);
		printf("fields: r %u/%u g %u/%u b %u/%u a %u/%u\n", var.red.offset, var.red.length, var.green.offset,
			   var.green.length, var.blue.offset, var.blue.length, var.transp.offset, var.transp.length);
		return 0;
	}

	var.yoffset = (unsigned)strtoul(argv[1], NULL, 0);
	if (ioctl(fd, FBIOPAN_DISPLAY, &var) < 0) {
		perror("FBIOPAN_DISPLAY");
		return 1;
	}
	printf("panned to yoffset %u\n", var.yoffset);
	return 0;
}
