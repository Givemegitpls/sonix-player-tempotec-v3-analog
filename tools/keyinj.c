// keyinj -- inject key events into an evdev node, so the player's button
// threads can be driven from a script instead of a finger.
//
//   keyinj <node> <code> [code ...]     press+release each code, 120 ms apart
//   keyinj <node> hold <code> <ms>      hold one code down for <ms>
//
// The codes are the kernel's KEY_* numbers (see linux/input.h on the host);
// the node on this device is /dev/input/event1 (gpio-keys).
//
// Built like the other tools here (see tools/README.md):
//   mipsel-rockbox-linux-gnu-gcc -static -O2 -o keyinj keyinj.c
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#define EV_SYN 0x00
#define EV_KEY 0x01
#define SYN_REPORT 0

struct input_event {
	struct timeval time;
	uint16_t type;
	uint16_t code;
	int32_t value;
};

static void emit(int fd, uint16_t type, uint16_t code, int32_t value) {
	struct input_event ev;
	memset(&ev, 0, sizeof(ev));
	gettimeofday(&ev.time, NULL);
	ev.type = type;
	ev.code = code;
	ev.value = value;
	if (write(fd, &ev, sizeof(ev)) != (ssize_t)sizeof(ev)) {
		fprintf(stderr, "keyinj: write: %s\n", strerror(errno));
		exit(1);
	}
}

static void press(int fd, uint16_t code) {
	emit(fd, EV_KEY, code, 1);
	emit(fd, EV_SYN, SYN_REPORT, 0);
}

static void release(int fd, uint16_t code) {
	emit(fd, EV_KEY, code, 0);
	emit(fd, EV_SYN, SYN_REPORT, 0);
}

int main(int argc, char **argv) {
	if (argc < 3) {
		fprintf(stderr, "usage: %s <node> <code>... | %s <node> hold <code> <ms>\n", argv[0], argv[0]);
		return 2;
	}
	int fd = open(argv[1], O_WRONLY);
	if (fd < 0) {
		fprintf(stderr, "keyinj: open %s: %s\n", argv[1], strerror(errno));
		return 1;
	}

	if (strcmp(argv[2], "hold") == 0) {
		if (argc < 5) {
			fprintf(stderr, "keyinj: hold needs a code and a duration\n");
			return 2;
		}
		uint16_t code = (uint16_t)strtoul(argv[3], NULL, 0);
		long ms = strtol(argv[4], NULL, 0);
		press(fd, code);
		usleep((useconds_t)(ms * 1000));
		release(fd, code);
		return 0;
	}

	for (int i = 2; i < argc; i++) {
		uint16_t code = (uint16_t)strtoul(argv[i], NULL, 0);
		press(fd, code);
		usleep(40 * 1000);
		release(fd, code);
		usleep(80 * 1000);
	}
	return 0;
}
