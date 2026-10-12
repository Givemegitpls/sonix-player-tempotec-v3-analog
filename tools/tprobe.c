// tprobe -- what the kernel reports from an input device, and nothing else.
//
// Written for the V3 Analog's touch panel (GT9XX-TS on /dev/input/event2). The
// question it answers cannot be answered from the player's log: when the screen
// has been blanked once and the interface then ignores touches, is the kernel
// still reporting them?
//
//   tprobe [device] [seconds]
//
// Events are printed as they arrive, one line each, with a running count, and a
// summary at the end. No events at all while a finger is on the glass means the
// controller itself is not reporting -- which is a different bug, with a
// different fix, from the interface not reading a device that is.
//
// Build (from the repository root):
//   rockbox-toolchain/bin/mipsel-rockbox-linux-gnu-gcc -static -O2 -o tprobe tprobe.c

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

int main(int argc, char **argv) {
	const char *path = argc > 1 ? argv[1] : "/dev/input/event2";
	int seconds = argc > 2 ? atoi(argv[2]) : 10;
	if (seconds <= 0) {
		seconds = 10;
	}

	int fd = open(path, O_RDONLY | O_NONBLOCK);
	if (fd < 0) {
		fprintf(stderr, "tprobe: %s: %s\n", path, strerror(errno));
		return 1;
	}

	char name[128] = {0};
	if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) < 0) {
		snprintf(name, sizeof(name), "(no name)");
	}
	printf("tprobe: %s is '%s'; reading for %d s\n", path, name, seconds);
	fflush(stdout);

	struct timeval began;
	gettimeofday(&began, NULL);

	long count = 0, keys = 0, abs = 0, syn = 0;
	for (;;) {
		struct input_event ev;
		ssize_t n = read(fd, &ev, sizeof(ev));
		if (n == (ssize_t)sizeof(ev)) {
			count++;
			if (ev.type == EV_KEY) {
				keys++;
				printf("  key  code=%u value=%d\n", ev.code, ev.value);
			} else if (ev.type == EV_ABS) {
				abs++;
				printf("  abs  code=%u value=%d\n", ev.code, ev.value);
			} else if (ev.type == EV_SYN) {
				syn++;
			}
			fflush(stdout);
		} else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
			fprintf(stderr, "tprobe: read: %s\n", strerror(errno));
			break;
		}

		struct timeval now;
		gettimeofday(&now, NULL);
		if (now.tv_sec - began.tv_sec >= seconds) {
			break;
		}
		usleep(20 * 1000);
	}

	printf("tprobe: %ld events in %d s (key %ld, abs %ld, syn %ld)\n", count, seconds, keys, abs, syn);
	close(fd);
	return count > 0 ? 0 : 2;
}
