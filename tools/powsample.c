// One line of power state every N seconds, for a discharge measurement.
//
// The point is to tell apart the states the player can be in while the screen
// is dark, which look identical from the outside:
//
//   - backlight 0 and the panel's rails off          -- what screen-off should be
//   - backlight 0 and LDO7/LDO8 still enabled        -- the panel is powered,
//                                                       and the SLCD keeps
//                                                       scanning, with the glass
//                                                       black anyway
//
// Both should show the same black screen, so the current draw is the only
// difference visible from userspace -- and it is real: 3.0 V + 1.8 V rails, the
// LCD clocks and a scanning controller do not run for free.
//
// What is recorded, per sample:
//
//   mono   CLOCK_MONOTONIC seconds -- does not count a suspend
//   boot   CLOCK_BOOTTIME seconds  -- does; boot - mono grows by the sleep
//   mA     axp_battery/current_now, negative while discharging
//   mV     axp_battery/voltage_now
//   pct    axp_battery/capacity
//   stat   axp_battery/status (Charging / Discharging / Full)
//   bl     backlight brightness
//   ldo7   1 while the lcd_3v3 rail is enabled
//   ldo8   1 while the lcd_1v8 rail is enabled
//   vsync  slcd_vsync interrupt count, cumulative -- the panel's own TE line,
//          which only ticks while the panel has power
//   cpu    /proc/stat busy jiffies, cumulative -- the SoC's own work
//   player sonix_player utime+stime, cumulative
//   rss    sonix_player RSS in kB
//
// The deltas are computed afterwards: between two samples, vsync tells whether
// the panel was scanning, cpu how busy the SoC was, and a jump in boot - mono
// is a suspend of that length.
//
// Build (static, for the device):
//
//   mipsel-rockbox-linux-gnu-gcc -static -O2 -march=mips32r2 -mno-madd4 \
//       -o powsample powsample.c
//
// Run on the device:
//
//   adb push powsample /data/powsample
//   adb shell 'nohup /data/powsample /data/powsample.log 20 >/dev/null 2>&1 &'
//
// and read /data/powsample.log afterwards. One line per sample, flushed.

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static const char *POWER = "/sys/class/power_supply/axp_battery";

static double clock_s(clockid_t id) {
	struct timespec ts;
	if (clock_gettime(id, &ts) != 0) {
		return -1;
	}
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

// Reads the first line of a sysfs file as a signed number; -1 when it is not
// there (a rail that is disabled still has its state file, an absent one does
// not).
static long read_long(const char *path) {
	FILE *f = fopen(path, "r");
	if (!f) {
		return -1;
	}
	long v = -1;
	if (fscanf(f, "%ld", &v) != 1) {
		v = -1;
	}
	fclose(f);
	return v;
}

static void read_text(const char *path, char *out, size_t size) {
	out[0] = '\0';
	FILE *f = fopen(path, "r");
	if (!f) {
		snprintf(out, size, "?");
		return;
	}
	if (!fgets(out, (int)size, f)) {
		out[0] = '\0';
	}
	fclose(f);
	size_t n = strlen(out);
	while (n && (out[n - 1] == '\n' || out[n - 1] == '\r')) {
		out[--n] = '\0';
	}
}

// The two panel rails, found by name: regulator numbers are a kernel detail.
static void lcd_rails(int *ldo7, int *ldo8) {
	*ldo7 = -1;
	*ldo8 = -1;
	DIR *d = opendir("/sys/class/regulator");
	if (!d) {
		return;
	}
	struct dirent *e;
	while ((e = readdir(d))) {
		if (strncmp(e->d_name, "regulator.", 10) != 0) {
			continue;
		}
		char dir[256], name[64], state[64], path[320];
		snprintf(dir, sizeof(dir), "/sys/class/regulator/%s", e->d_name);
		snprintf(path, sizeof(path), "%s/name", dir);
		read_text(path, name, sizeof(name));
		int *slot = strcmp(name, "LDO7") == 0 ? ldo7 : strcmp(name, "LDO8") == 0 ? ldo8 : NULL;
		if (!slot) {
			continue;
		}
		snprintf(path, sizeof(path), "%s/state", dir);
		read_text(path, state, sizeof(state));
		if (strcmp(state, "disabled") == 0) {
			*slot = 0;
		} else if (state[0]) {
			*slot = 1;
		}
	}
	closedir(d);
}

// The panel's TE line: `slcd_vsync` in /proc/interrupts, cumulative.
static long slcd_vsync_count(void) {
	FILE *f = fopen("/proc/interrupts", "r");
	if (!f) {
		return -1;
	}
	char line[512];
	long total = 0;
	while (fgets(line, sizeof(line), f)) {
		if (!strstr(line, "slcd")) {
			continue;
		}
		// " 12:   123456   jz-intc  slcd_vsync"
		char *p = strchr(line, ':');
		if (!p) {
			continue;
		}
		total += strtol(p + 1, NULL, 10);
	}
	fclose(f);
	return total;
}

// Cumulative busy jiffies over all CPUs, from /proc/stat's `cpu` line.
static long cpu_busy(void) {
	FILE *f = fopen("/proc/stat", "r");
	if (!f) {
		return -1;
	}
	char line[256];
	long v[10] = {0};
	long busy = -1;
	if (fgets(line, sizeof(line), f) && strncmp(line, "cpu ", 4) == 0) {
		int n = sscanf(line + 4, "%ld %ld %ld %ld %ld %ld %ld", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6]);
		if (n >= 4) {
			busy = v[0] + v[1] + v[2] + v[5] + v[6];
		}
	}
	fclose(f);
	return busy;
}

// utime+stime of one task, from its /proc/.../stat: the fields after the
// command name, counted from field 1 in the kernel's own numbering.
static long task_ticks(const char *stat_path) {
	FILE *f = fopen(stat_path, "r");
	if (!f) {
		return -1;
	}
	char buf[1024];
	if (!fgets(buf, sizeof(buf), f)) {
		fclose(f);
		return -1;
	}
	fclose(f);
	char *close = strrchr(buf, ')');
	if (!close) {
		return -1;
	}
	long utime = 0, stime = 0;
	// After ") " come fields 3..: state, ppid, ... utime is field 14, stime 15.
	int n = sscanf(close + 2,
				   "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %ld %ld",
				   &utime, &stime);
	if (n != 2) {
		return -1;
	}
	return utime + stime;
}

// sonix_player's own cpu time and RSS: the sum over its threads (the thread
// group's cpu time is not in /proc/<pid>/stat for a multithreaded process).
static void player_stat(long *ticks, long *rss_kb) {
	*ticks = -1;
	*rss_kb = -1;
	DIR *d = opendir("/proc");
	if (!d) {
		return;
	}
	struct dirent *e;
	long pid = -1;
	while ((e = readdir(d))) {
		if (e->d_name[0] < '0' || e->d_name[0] > '9') {
			continue;
		}
		char path[256], comm[64];
		snprintf(path, sizeof(path), "/proc/%s/comm", e->d_name);
		read_text(path, comm, sizeof(comm));
		if (strcmp(comm, "sonix_player") == 0) {
			pid = strtol(e->d_name, NULL, 10);
			break;
		}
	}
	closedir(d);
	if (pid < 0) {
		return;
	}

	char path[256];
	snprintf(path, sizeof(path), "/proc/%ld/status", pid);
	FILE *f = fopen(path, "r");
	if (f) {
		char line[256];
		while (fgets(line, sizeof(line), f)) {
			if (strncmp(line, "VmRSS:", 6) == 0) {
				*rss_kb = strtol(line + 6, NULL, 10);
				break;
			}
		}
		fclose(f);
	}

	long total = 0;
	snprintf(path, sizeof(path), "/proc/%ld/task", pid);
	DIR *t = opendir(path);
	if (!t) {
		return;
	}
	while ((e = readdir(t))) {
		if (e->d_name[0] < '0' || e->d_name[0] > '9') {
			continue;
		}
		char stat[320];
		snprintf(stat, sizeof(stat), "%s/%s/stat", path, e->d_name);
		long v = task_ticks(stat);
		if (v > 0) {
			total += v;
		}
	}
	closedir(t);
	*ticks = total;
}

int main(int argc, char **argv) {
	if (argc < 2) {
		fprintf(stderr, "usage: %s <logfile> [interval-seconds]\n", argv[0]);
		return 2;
	}
	const char *log_path = argv[1];
	int interval = argc > 2 ? atoi(argv[2]) : 20;
	if (interval < 5) {
		interval = 5;
	}

	FILE *log = fopen(log_path, "a");
	if (!log) {
		fprintf(stderr, "%s: %s\n", log_path, strerror(errno));
		return 1;
	}
	setvbuf(log, NULL, _IOLBF, 0);

	char cur_path[256], volt_path[256], cap_path[256], stat_path[256];
	snprintf(cur_path, sizeof(cur_path), "%s/current_now", POWER);
	snprintf(volt_path, sizeof(volt_path), "%s/voltage_now", POWER);
	snprintf(cap_path, sizeof(cap_path), "%s/capacity", POWER);
	snprintf(stat_path, sizeof(stat_path), "%s/status", POWER);

	fprintf(log, "# mono boot mA mV pct stat bl ldo7 ldo8 vsync cpu player rss\n");
	fflush(log);

	for (;;) {
		double mono = clock_s(CLOCK_MONOTONIC);
		double boot = clock_s(CLOCK_BOOTTIME);
		long ma = read_long(cur_path);
		long mv = read_long(volt_path);
		long pct = read_long(cap_path);
		char status[32];
		read_text(stat_path, status, sizeof(status));
		long bl = read_long("/sys/class/backlight/pwm-backlight.0/brightness");
		int ldo7 = -1, ldo8 = -1;
		lcd_rails(&ldo7, &ldo8);
		long vsync = slcd_vsync_count();
		long cpu = cpu_busy();
		long pticks = -1, rss = -1;
		player_stat(&pticks, &rss);

		fprintf(log, "%.3f %.3f %ld %ld %ld %s %ld %d %d %ld %ld %ld %ld\n", mono, boot, ma, mv, pct, status, bl,
				ldo7, ldo8, vsync, cpu, pticks, rss);
		fflush(log);

		// Sleep in one call: the sampler should not add wakeups of its own to
		// the thing it measures.
		struct timespec ts = {.tv_sec = interval, .tv_nsec = 0};
		nanosleep(&ts, NULL);
	}
}
