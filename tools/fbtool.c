// fbtool -- drive /dev/fb0 and read the LCDC/SLCDC registers from /dev/mem,
// so the SLCD output path can be watched and poked from userland.
//
// Commands (processed left to right, one fb fd held open unless closed):
//   info            print fb var/fix
//   blank N         FBIOBLANK (0 unblank, 4 powerdown)
//   unblank         FBIOBLANK 0
//   put             FBIOGET_VSCREENINFO then FBIOPUT_VSCREENINFO (ACTIVATE_NOW|FORCE)
//   pan Y           set yoffset Y, FBIOPAN_DISPLAY
//   show            FBIOGET_VSCREENINFO and print yoffset
//   open            open a second /dev/fb0 fd (kept in slot 1)
//   close           close the second fd
//   sleep MS
//   regs            print LCDC/SLCDC registers via /dev/mem
//   irq             print the lcdc and slcd_vsync lines of /proc/interrupts
//   watch MS STEP   sample irq+regs every STEP ms for MS ms
//   wreg OFF VAL    write a 32-bit register (physical offset from LCDC base)
//
// Static on purpose: the device glibc is older than the toolchain's.
//
// Build (from sonix-player):
//   rockbox-toolchain/bin/mipsel-rockbox-linux-gnu-gcc -static -O2 -o fbtool fbtool.c

#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define LCDC_BASE 0x13050000u // X1000 LCDC registers (matches the driver's dump)

static int fb_fd = -1;
static int fb_fd2 = -1;
static struct fb_var_screeninfo var;
static volatile uint32_t *regs = NULL;

static void regs_map(void) {
	if (regs) {
		return;
	}
	int fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (fd < 0) {
		perror("/dev/mem");
		return;
	}
	void *m = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, LCDC_BASE);
	close(fd);
	if (m == MAP_FAILED) {
		perror("mmap /dev/mem");
		return;
	}
	regs = m;
}

static void do_irq(void) {
	FILE *f = fopen("/proc/interrupts", "r");
	if (!f) {
		return;
	}
	char line[512];
	while (fgets(line, sizeof(line), f)) {
		if (strstr(line, "lcdc") || strstr(line, "slcd_vsync")) {
			printf("irq: %s", line);
		}
	}
	fclose(f);
}

static void do_regs(void) {
	regs_map();
	if (!regs) {
		return;
	}
	uint32_t ctrl = regs[0x30 / 4], state = regs[0x34 / 4];
	uint32_t cfg = regs[0x00 / 4];
	uint32_t scfg = regs[0xa0 / 4], sctrl = regs[0xa4 / 4], sstate = regs[0xa8 / 4];
	uint32_t da0 = regs[0x40 / 4], fid0 = regs[0x48 / 4], cmd0 = regs[0x4c / 4];
	printf("regs: LCDC_CFG=%08x LCDC_CTRL=%08x(ENA=%d) LCDC_STATE=%08x | "
		   "SLCDC_CFG=%08x SLCDC_CTRL=%08x(DMA_EN=%d START=%d MODE=%d NEW=%d FAST=%d NOTE=%d GATE=%d MIPI=%d) SLCDC_STATE=%08x BUSY=%d | "
		   "DA0=%08x FID0=%08x CMD0=%08x\n",
		   cfg, ctrl, (ctrl >> 3) & 1, state,
		   scfg, sctrl, sctrl & 1, (sctrl >> 1) & 1, (sctrl >> 2) & 1, (sctrl >> 5) & 1,
		   (sctrl >> 4) & 1, (sctrl >> 8) & 1, (sctrl >> 3) & 1, (sctrl >> 6) & 1,
		   sstate, sstate & 1, da0, fid0, cmd0);
}

static void do_info(void) {
	struct fb_fix_screeninfo fix;
	if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &var) < 0) {
		perror("GET_VSCREENINFO");
		return;
	}
	ioctl(fb_fd, FBIOGET_FSCREENINFO, &fix);
	printf("var: %ux%u virt %ux%u bpp %u yoff %u activate %u | fix: line %u smem_len %u mmio %08x\n",
		   var.xres, var.yres, var.xres_virtual, var.yres_virtual, var.bits_per_pixel, var.yoffset, var.activate,
		   fix.line_length, fix.smem_len, fix.mmio_start);
}

int main(int argc, char **argv) {
	fb_fd = open("/dev/fb0", O_RDWR);
	if (fb_fd < 0) {
		perror("/dev/fb0");
		return 1;
	}
	ioctl(fb_fd, FBIOGET_VSCREENINFO, &var);

	for (int i = 1; i < argc; i++) {
		const char *c = argv[i];
		if (!strcmp(c, "info")) {
			do_info();
		} else if (!strcmp(c, "blank")) {
			long n = i + 1 < argc ? strtol(argv[++i], NULL, 0) : 0;
			if (ioctl(fb_fd, FBIOBLANK, n) < 0) {
				perror("FBIOBLANK");
			} else {
				printf("blank %ld ok\n", n);
			}
		} else if (!strcmp(c, "unblank")) {
			if (ioctl(fb_fd, FBIOBLANK, FB_BLANK_UNBLANK) < 0) {
				perror("FBIOBLANK");
			} else {
				printf("unblank ok\n");
			}
		} else if (!strcmp(c, "put")) {
			ioctl(fb_fd, FBIOGET_VSCREENINFO, &var);
			var.activate = FB_ACTIVATE_NOW | FB_ACTIVATE_FORCE;
			if (ioctl(fb_fd, FBIOPUT_VSCREENINFO, &var) < 0) {
				perror("FBIOPUT_VSCREENINFO");
			} else {
				printf("put ok (yoff %u virt %ux%u)\n", var.yoffset, var.xres_virtual, var.yres_virtual);
			}
		} else if (!strcmp(c, "pan")) {
			long y = i + 1 < argc ? strtol(argv[++i], NULL, 0) : 0;
			var.yoffset = (uint32_t)y;
			if (ioctl(fb_fd, FBIOPAN_DISPLAY, &var) < 0) {
				perror("FBIOPAN_DISPLAY");
			} else {
				printf("pan %ld ok\n", y);
			}
		} else if (!strcmp(c, "show")) {
			struct fb_var_screeninfo v;
			ioctl(fb_fd, FBIOGET_VSCREENINFO, &v);
			printf("show: yoff %u virt %ux%u\n", v.yoffset, v.xres_virtual, v.yres_virtual);
		} else if (!strcmp(c, "open")) {
			fb_fd2 = open("/dev/fb0", O_RDWR);
			printf("open2 -> %d (%s)\n", fb_fd2, fb_fd2 < 0 ? strerror(errno) : "ok");
		} else if (!strcmp(c, "close")) {
			if (fb_fd2 >= 0) {
				close(fb_fd2);
				fb_fd2 = -1;
				printf("close2 ok\n");
			}
		} else if (!strcmp(c, "sleep")) {
			long ms = i + 1 < argc ? strtol(argv[++i], NULL, 0) : 0;
			usleep((useconds_t)ms * 1000);
		} else if (!strcmp(c, "regs")) {
			do_regs();
		} else if (!strcmp(c, "irq")) {
			do_irq();
		} else if (!strcmp(c, "watch")) {
			long ms = i + 2 < argc ? strtol(argv[++i], NULL, 0) : 1000;
			long step = i + 1 < argc ? strtol(argv[++i], NULL, 0) : 200;
			for (long t = 0; t < ms; t += step) {
				do_irq();
				do_regs();
				usleep((useconds_t)step * 1000);
			}
		} else if (!strcmp(c, "poll")) {
			long n = i + 1 < argc ? strtol(argv[++i], NULL, 0) : 2000;
			regs_map();
			if (regs) {
				long busy = 0, sstate_changes = 0, lcdc_changes = 0;
				uint32_t prev = regs[0xa8 / 4], prevl = regs[0x34 / 4];
				for (long k = 0; k < n; k++) {
					uint32_t s = regs[0xa8 / 4], l = regs[0x34 / 4];
					if (s & 1) {
						busy++;
					}
					if (s != prev) {
						sstate_changes++;
						prev = s;
					}
					if (l != prevl) {
						lcdc_changes++;
						prevl = l;
					}
				}
				printf("poll %ld: SLCDC_STATE busy=%ld changes=%ld | LCDC_STATE changes=%ld\n", n, busy, sstate_changes,
					   lcdc_changes);
			}
		} else if (!strcmp(c, "wreg")) {
			unsigned off = i + 2 < argc ? (unsigned)strtoul(argv[++i], NULL, 0) : 0;
			unsigned val = i + 1 < argc ? (unsigned)strtoul(argv[++i], NULL, 0) : 0;
			regs_map();
			if (regs) {
				regs[off / 4] = val;
				printf("wreg %03x <- %08x\n", off, val);
			}
		} else {
			fprintf(stderr, "unknown command: %s\n", c);
			return 2;
		}
	}
	if (fb_fd2 >= 0) {
		close(fb_fd2);
	}
	close(fb_fd);
	return 0;
}
