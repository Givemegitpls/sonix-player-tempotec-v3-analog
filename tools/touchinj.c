// touchinj -- inject one touch into an evdev node, so the interface can be
// driven from a script instead of a finger.
//
//   touchinj <node> <x> <y> [hold_ms]        a tap at (x, y)
//   touchinj <node> drag <x1> <y1> <x2> <y2> [ms]   press, move, release
//
// Written for the V3 Analog's panel (GT9XX-TS on /dev/input/event2). The
// controller declares the multi-touch axes only -- there is no ABS_X/ABS_Y --
// so the events are protocol B: a slot, a tracking id, the position, and the
// release that lifts the finger. That is what lv_evdev reads.
//
// The tap is written the way a real one arrives: press, SYN, the hold, then a
// tracking id of -1, SYN. A tap that never lifts leaves the interface dragging.
//
// Build (from the repository root):
//   rockbox-toolchain/bin/mipsel-rockbox-linux-gnu-gcc -static -O2 -o touchinj touchinj.c

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

// The codes used here have not moved since
// 3.10: EV_SYN 0, EV_KEY 1, EV_ABS 3, ABS_MT_SLOT 0x2f, ABS_MT_TRACKING_ID 0x39,
// ABS_MT_POSITION_X 0x35, ABS_MT_POSITION_Y 0x36, BTN_TOUCH 0x14a.
#define EV_SYN 0x00
#define EV_KEY 0x01
#define EV_ABS 0x03

#define SYN_REPORT 0

#define ABS_MT_TOUCH_MAJOR 0x30
#define ABS_MT_POSITION_X 0x35
#define ABS_MT_POSITION_Y 0x36
#define ABS_MT_TRACKING_ID 0x39



struct input_event {
	struct timeval time;
	uint16_t type;
	uint16_t code;
	int32_t value;
};

static int out = -1;

static void emit(uint16_t type, uint16_t code, int32_t value) {
	struct input_event ev;
	memset(&ev, 0, sizeof(ev));
	gettimeofday(&ev.time, NULL);
	ev.type = type;
	ev.code = code;
	ev.value = value;
	if (write(out, &ev, sizeof(ev)) != (ssize_t)sizeof(ev)) {
		fprintf(stderr, "touchinj: write: %s\n", strerror(errno));
		exit(1);
	}
}

static void sync_report(void) { emit(EV_SYN, SYN_REPORT, 0); }

static void sleep_ms(int ms) {
	if (ms > 0) {
		usleep((useconds_t)ms * 1000);
	}
}

static void press(int x, int y, int id) {
	// Protocol A: this controller declares no ABS_MT_SLOT, so one contact is
	// reported per frame and the tracking id is what says it is the same
	// finger. ABS_MT_TOUCH_MAJOR is declared and a zero-width contact is
	// treated as lifted by some readers, so it is set to something.
	emit(EV_ABS, ABS_MT_TRACKING_ID, id);
	emit(EV_ABS, ABS_MT_POSITION_X, x);
	emit(EV_ABS, ABS_MT_POSITION_Y, y);
	emit(EV_ABS, ABS_MT_TOUCH_MAJOR, 10);
	sync_report();
}

static void move_to(int x, int y) {
	emit(EV_ABS, ABS_MT_POSITION_X, x);
	emit(EV_ABS, ABS_MT_POSITION_Y, y);
	emit(EV_ABS, ABS_MT_TOUCH_MAJOR, 10);
	sync_report();
}

static void release(void) {
	emit(EV_ABS, ABS_MT_TRACKING_ID, -1);
	sync_report();
}

int main(int argc, char **argv) {
	if (argc < 4) {
		fprintf(stderr, "usage: %s <node> <x> <y> [hold_ms]\n"
						"       %s <node> drag <x1> <y1> <x2> <y2> [ms]\n",
				argv[0], argv[0]);
		return 2;
	}

	const char *path = argv[1];
	out = open(path, O_WRONLY);
	if (out < 0) {
		fprintf(stderr, "touchinj: %s: %s\n", path, strerror(errno));
		return 1;
	}

	if (strcmp(argv[2], "drag") == 0) {
		if (argc < 7) {
			fprintf(stderr, "touchinj: drag wants x1 y1 x2 y2 [ms]\n");
			return 2;
		}
		int x1 = atoi(argv[3]), y1 = atoi(argv[4]);
		int x2 = atoi(argv[5]), y2 = atoi(argv[6]);
		int ms = argc > 7 ? atoi(argv[7]) : 200;

		press(x1, y1, 100);
		sleep_ms(40);
		// A handful of intermediate reports, so the interface sees a movement
		// rather than a jump it may treat as two separate gestures.
		const int steps = 8;
		for (int i = 1; i <= steps; i++) {
			move_to(x1 + (x2 - x1) * i / steps, y1 + (y2 - y1) * i / steps);
			sleep_ms(ms / steps);
		}
		release();
	} else {
		int x = atoi(argv[2]), y = atoi(argv[3]);
		int hold = argc > 4 ? atoi(argv[4]) : 60;

		press(x, y, 100);
		sleep_ms(hold);
		release();
	}

	close(out);
	return 0;
}
