// What does JZFB_SET_VSYNCINT (ioctl 0x4610) actually do?
//
// The stock player uses it for frame synchronisation and the port does not use
// it at all, and the port's copy is therefore not held back by the panel: the
// flip thread issues FBIOPAN_DISPLAY and clears its flag at once, so the next
// frame's copy starts while the panel's DMA may still be reading the same page.
// That is tearing, and it is visible exactly where the content changes every
// frame -- scrolling a list, opening the quick panel.
//
// The vendor driver is only partly documented (see NOTES.md: the platform data,
// the panel table, the blank and suspend paths are all read out of kernel.bin,
// but the ioctl handler is not). This measures the one thing that decides how to
// use it:
//
//   * if a call blocks for about one panel frame (13 ms at the panel's 75 Hz),
//     it waits for the next vsync and can be called before each copy;
//   * if it returns at once, it arms or configures something, and the wait has
//     to come from somewhere else.
//
// It also reports the slcd_vsync interrupt count before and after, so arming an
// interrupt is visible even when the call does not block.
//
// Build (static, for the device):
//
//   mipsel-rockbox-linux-gnu-gcc -static -O2 -march=mips32r2 -mno-madd4 -o vsynctest vsynctest.c
//
// Run it from the card (/data is not exec on this device). The player can be
// running: this only asks the driver questions.
//
//   adb push vsynctest /mnt/sd_0/ && adb shell 'chmod +x /mnt/sd_0/vsynctest'
//   adb shell '/mnt/sd_0/vsynctest 1 20'    # 20 calls with argument 1
//   adb shell '/mnt/sd_0/vsynctest 0 20'    # and with 0, 2, ...

#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define JZFB_SET_VSYNCINT 0x4610

static double now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

// The line of /proc/interrupts that counts the panel's own frame interrupt.
static unsigned long vsync_count(void) {
	FILE *f = fopen("/proc/interrupts", "r");
	if (!f) {
		return 0;
	}
	char line[512];
	unsigned long n = 0;
	while (fgets(line, sizeof(line), f)) {
		if (strstr(line, "slcd_vsync")) {
			unsigned long v = 0;
			if (sscanf(line, " %*d: %lu", &v) == 1) {
				n = v;
			}
		}
	}
	fclose(f);
	return n;
}

int main(int argc, char **argv) {
	bool by_pointer = argc > 1 && strcmp(argv[1], "ptr") == 0;
	int shift = by_pointer ? 1 : 0;
	unsigned long arg = argc > 1 + shift ? strtoul(argv[1 + shift], NULL, 0) : 1;
	int calls = argc > 2 + shift ? atoi(argv[2 + shift]) : 20;
	unsigned long cmd = argc > 3 + shift ? strtoul(argv[3 + shift], NULL, 0) : JZFB_SET_VSYNCINT;
	int value = (int)arg;

	int fd = open("/dev/fb0", O_RDWR);
	if (fd < 0) {
		perror("/dev/fb0");
		return 1;
	}

	unsigned long irq0 = vsync_count();
	printf("ioctl 0x%lx, argument %s%lu, %d calls; slcd_vsync at %lu\n", cmd, by_pointer ? "&" : "", arg, calls, irq0);

	double first = 0, worst = 0, total = 0;
	int fails = 0;
	for (int i = 0; i < calls; i++) {
		double t0 = now_ms();
		int r = by_pointer ? ioctl(fd, cmd, &value) : ioctl(fd, cmd, arg);
		double dt = now_ms() - t0;
		if (r != 0) {
			fails++;
			printf("  call %2d: -> %d in %.3f ms\n", i, r, dt);
		}
		if (i == 0) {
			first = dt;
		}
		if (dt > worst) {
			worst = dt;
		}
		total += dt;
	}
	unsigned long irq1 = vsync_count();

	printf("first call %.3f ms, worst %.3f ms, average %.3f ms, failures %d\n", first, worst, total / calls, fails);
	printf("slcd_vsync %lu -> %lu (%lu interrupts during the run)\n", irq0, irq1, irq1 - irq0);
	printf("a wait-for-vsync would average about 6.7 ms (a panel frame is 13.3 ms);\n"
		   "an instant return means it only configures something.\n");

	close(fd);
	return 0;
}
