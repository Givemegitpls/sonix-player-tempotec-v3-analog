// What the panel's DMA is doing, microsecond by microsecond, around a pan.
//
// The port's single-page path writes the framebuffer page that the panel's DMA
// reads, and it does not wait for that read to finish: the flip thread issues
// FBIOPAN_DISPLAY and clears its flag at once. If the transfer is still running
// when the copy starts, part of the glass shows the new frame and part the old
// one -- tearing, and it is visible exactly where the content changes every
// frame (scrolling a list, opening the quick panel).
//
// The registers say whether the transfer is running (see NOTES.md):
//
//   SLCDC_CTRL  (+0xa4): bit0 DMA_EN, bit1 DMA_START, bit2 DMA_MODE
//   SLCDC_STATE (+0xa8): bit0 BUSY
//
// This samples them as fast as it can around one pan, so the answer to "can the
// copy wait for the transfer to finish" is a measurement rather than a guess:
// how long the transfer takes, and which of the two bits is the one that clears
// when it is done.
//
// Build (static, for the device):
//
//   mipsel-rockbox-linux-gnu-gcc -static -O2 -march=mips32r2 -mno-madd4 -o panwatch panwatch.c
//
// Run from the card, with the player running:
//
//   adb shell '/mnt/sd_0/panwatch 200000 100000'   # samples, window in us

#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define LCDC_BASE 0x13050000u
#define SLCDC_CTRL_OFF 0xa4u
#define SLCDC_STATE_OFF 0xa8u

static volatile uint32_t *regs;
static struct timespec t0;

static double us_since_start(void) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (double)(now.tv_sec - t0.tv_sec) * 1e6 + (double)(now.tv_nsec - t0.tv_nsec) / 1e3;
}

int main(int argc, char **argv) {
	int samples = argc > 1 ? atoi(argv[1]) : 4000;

	int fd = open("/dev/fb0", O_RDWR);
	if (fd < 0) {
		perror("/dev/fb0");
		return 1;
	}
	int mem = open("/dev/mem", O_RDWR | O_SYNC);
	if (mem < 0) {
		perror("/dev/mem");
		return 1;
	}
	void *m = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, mem, LCDC_BASE);
	close(mem);
	if (m == MAP_FAILED) {
		perror("mmap /dev/mem");
		return 1;
	}
	regs = m;

	struct fb_var_screeninfo var;
	if (ioctl(fd, FBIOGET_VSCREENINFO, &var) != 0) {
		perror("FBIOGET_VSCREENINFO");
		return 1;
	}

	uint32_t ctrl = regs[SLCDC_CTRL_OFF / 4];
	uint32_t state = regs[SLCDC_STATE_OFF / 4];
	printf("before the pan: SLCDC_CTRL=0x%x (DMA_EN=%u DMA_START=%u), SLCDC_STATE=0x%x (BUSY=%u)\n", ctrl,
		   ctrl & 1u, (ctrl >> 1) & 1u, state, state & 1u);

	clock_gettime(CLOCK_MONOTONIC, &t0);
	if (ioctl(fd, FBIOPAN_DISPLAY, &var) != 0) {
		perror("FBIOPAN_DISPLAY");
		return 1;
	}
	double after_ioctl = us_since_start();

	// Sample both registers as fast as the CPU will read them, and print only
	// the changes: the timeline of a transfer is a handful of edges.
	uint32_t prev_ctrl = regs[SLCDC_CTRL_OFF / 4], prev_state = regs[SLCDC_STATE_OFF / 4];
	printf("%8.1f us  pan issued: CTRL=0x%x STATE=0x%x\n", after_ioctl, prev_ctrl, prev_state);

	uint32_t busy_seen = 0, start_seen = 0;
	double last_change = after_ioctl;
	for (int i = 0; i < samples; i++) {
		uint32_t c = regs[SLCDC_CTRL_OFF / 4];
		uint32_t s = regs[SLCDC_STATE_OFF / 4];
		if ((c & 1u) == 0) {
			busy_seen++; // DMA_EN dropped: the panel is no longer being driven
		}
		if ((c >> 1) & 1u) {
			start_seen++;
		}
		if (c != prev_ctrl || s != prev_state) {
			double t = us_since_start();
			printf("%8.1f us  CTRL=0x%x (DMA_EN=%u DMA_START=%u) STATE=0x%x (BUSY=%u)\n", t, c, c & 1u,
				   (c >> 1) & 1u, s, s & 1u);
			prev_ctrl = c;
			prev_state = s;
			last_change = t;
		}
		if (us_since_start() > (double)(argc > 2 ? atoi(argv[2]) : 30000)) {
			break;
		}
	}

	printf("sampled %d times; DMA_START was set in %u samples, DMA_EN was 0 in %u\n", samples, start_seen,
		   busy_seen);
	printf("last change %.1f us after the pan\n", last_change);
	return 0;
}
