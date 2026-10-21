// The stock player's own panel sleep, as one command.
//
// docs/PANEL-POWER.md: on this board the stock player does not blank the
// framebuffer -- it sends the vendor ioctl SET_SLEEP (0x80044620, _IOW('F',
// 0x20, int)) to /dev/fb0. The kernel's jzfb_suspend then waits for the SLCDC
// transfer, clears LCDC_CTRL.ENA, gates the LCD clocks, disables the AXP2101
// rails LDO7 (lcd_3v3) and LDO8 (lcd_1v8), drives gpio16 (lcd rst) and gpio48
// (lcd rd) low and puts the touch controller to doze. The same ioctl with 0
// runs the reverse and ends with a pan, so the glass comes back with a frame.
//
// This tool is the measurement, not the fix: it calls that ioctl and prints
// what changed around it, so the port can be wired to the path the stock
// firmware actually uses (src/system/device/power.c currently only writes the
// backlight). Run it from the card; it does not touch the player's files and
// needs nothing flashed.
//
// Build (static, for the device):
//
//   mipsel-rockbox-linux-gnu-gcc -static -O2 -march=mips32r2 -mno-madd4 -o sleepfb sleepfb.c
//
// Run from the card:
//
//   adb shell '/mnt/sd_0/sleepfb status'
//   adb shell '/mnt/sd_0/sleepfb off'         # panel down; backlight unchanged
//   adb shell '/mnt/sd_0/sleepfb on'          # panel up, pan, frame back
//   adb shell '/mnt/sd_0/sleepfb cycle 5'     # off, wait 5 s, on
//
// The backlight is deliberately left alone: on the stock player it is a
// separate write, and this tool is about the panel. A black glass with the
// backlight on is the expected intermediate state.

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define LCDC_IOBASE 0x13050000u
#define LCDC_CTRL_OFF 0x30u
#define LCDC_STATE_OFF 0x34u
#define SLCDC_CTRL_OFF 0xa4u
#define SLCDC_STATE_OFF 0xa8u

#define LCDC_CTRL_ENA (1u << 3)
#define SLCDC_CTRL_DMA_EN (1u << 0)
#define SLCDC_CTRL_DMA_START (1u << 1)
#define SLCDC_STATE_BUSY (1u << 0)

// _IOW('F', 0x20, int) on MIPS: direction 1 (write), size 4, type 'F', nr 0x20.
#define JZFB_SET_SLEEP 0x80044620u

static volatile uint32_t *regs;

static uint32_t reg_read(uint32_t off) { return regs[off / 4]; }

static double now_us(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec * 1e6 + (double)ts.tv_nsec / 1e3;
}

static void regs_dump(const char *when) {
	uint32_t ctrl = reg_read(LCDC_CTRL_OFF);
	uint32_t state = reg_read(LCDC_STATE_OFF);
	uint32_t sctrl = reg_read(SLCDC_CTRL_OFF);
	uint32_t sstate = reg_read(SLCDC_STATE_OFF);

	printf("%-6s LCDC_CTRL=%08x ENA=%u LCDC_STATE=%08x | "
		   "SLCDC_CTRL=%08x DMA_EN=%u DMA_START=%u SLCDC_STATE=%08x BUSY=%u\n",
		   when, ctrl, (ctrl & LCDC_CTRL_ENA) ? 1u : 0u, state, sctrl,
		   (sctrl & SLCDC_CTRL_DMA_EN) ? 1u : 0u, (sctrl & SLCDC_CTRL_DMA_START) ? 1u : 0u, sstate,
		   (sstate & SLCDC_STATE_BUSY) ? 1u : 0u);
}

// The rails are regulators in the kernel; the report says they have no enable
// attribute in sysfs, but the name and (when the driver exposes one) the state
// and microvolts are still worth recording before/after.
static void rails_dump(void) {
	DIR *dir = opendir("/sys/class/regulator");
	if (!dir) {
		printf("rails: /sys/class/regulator: %s\n", strerror(errno));
		return;
	}

	struct dirent *entry;
	bool any = false;
	while ((entry = readdir(dir)) != NULL) {
		if (strncmp(entry->d_name, "regulator", 9) != 0) {
			continue;
		}

		char path[512];
		char name[64] = "";
		snprintf(path, sizeof(path), "/sys/class/regulator/%s/name", entry->d_name);
		FILE *f = fopen(path, "r");
		if (!f) {
			continue;
		}
		if (!fgets(name, sizeof(name), f)) {
			name[0] = '\0';
		}
		fclose(f);
		name[strcspn(name, "\n")] = '\0';

		if (strstr(name, "lcd") == NULL && strstr(name, "LDO7") == NULL && strstr(name, "LDO8") == NULL &&
			strstr(name, "ldo7") == NULL && strstr(name, "ldo8") == NULL) {
			continue;
		}

		char state[32] = "?";
		char volts[32] = "?";
		snprintf(path, sizeof(path), "/sys/class/regulator/%s/state", entry->d_name);
		f = fopen(path, "r");
		if (f) {
			if (fgets(state, sizeof(state), f)) {
				state[strcspn(state, "\n")] = '\0';
			}
			fclose(f);
		}
		snprintf(path, sizeof(path), "/sys/class/regulator/%s/microvolts", entry->d_name);
		f = fopen(path, "r");
		if (f) {
			if (fgets(volts, sizeof(volts), f)) {
				volts[strcspn(volts, "\n")] = '\0';
			}
			fclose(f);
		}
		printf("rails: %-10s state=%-10s microvolts=%s\n", name, state, volts);
		any = true;
	}
	closedir(dir);
	if (!any) {
		printf("rails: no lcd/LDO7/LDO8 regulator in sysfs\n");
	}
}

static void backlight_dump(void) {
	const char *path = "/sys/class/backlight/pwm-backlight.0/brightness";
	FILE *f = fopen(path, "r");
	if (!f) {
		printf("backlight: %s: %s\n", path, strerror(errno));
		return;
	}
	char line[32] = "?";
	if (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\n")] = '\0';
	}
	fclose(f);
	printf("backlight: %s\n", line);
}

static bool set_sleep(int fd, int value) {
	double t0 = now_us();
	int rc = ioctl(fd, JZFB_SET_SLEEP, &value);
	double dt = now_us() - t0;
	int saved = errno;

	regs_dump("after:");
	rails_dump();
	backlight_dump();
	if (rc != 0) {
		printf("SET_SLEEP(%d): failed: %s (errno %d), %.1f ms\n", value, strerror(saved), saved, dt / 1000.0);
		return false;
	}
	printf("SET_SLEEP(%d): ok, %.1f ms\n", value, dt / 1000.0);
	return true;
}

int main(int argc, char **argv) {
	const char *action = argc > 1 ? argv[1] : "status";
	int fd = -1;

	if (strcmp(action, "status") != 0) {
		fd = open("/dev/fb0", O_RDWR);
		if (fd < 0) {
			perror("/dev/fb0");
			return 1;
		}
	}

	int mem = open("/dev/mem", O_RDWR | O_SYNC);
	if (mem < 0) {
		perror("/dev/mem");
		return 1;
	}
	void *m = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, mem, LCDC_IOBASE);
	close(mem);
	if (m == MAP_FAILED) {
		perror("mmap /dev/mem");
		return 1;
	}
	regs = m;

	printf("before: ");
	regs_dump("");
	rails_dump();
	backlight_dump();

	if (strcmp(action, "status") == 0) {
		return 0;
	}

	if (strcmp(action, "off") == 0) {
		return set_sleep(fd, 1) ? 0 : 1;
	}

	if (strcmp(action, "on") == 0) {
		return set_sleep(fd, 0) ? 0 : 1;
	}

	if (strcmp(action, "cycle") == 0) {
		int seconds = argc > 2 ? atoi(argv[2]) : 3;
		if (seconds <= 0) {
			seconds = 3;
		}
		if (!set_sleep(fd, 1)) {
			return 1;
		}
		printf("sleeping %d s\n", seconds);
		sleep((unsigned)seconds);
		return set_sleep(fd, 0) ? 0 : 1;
	}

	printf("usage: %s status|off|on|cycle [seconds]\n", argv[0]);
	return 2;
}
