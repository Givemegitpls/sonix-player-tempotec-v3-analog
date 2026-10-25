#include "profiler.h"

#include <dirent.h>
#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <ucontext.h>
#include <unistd.h>

#define PROFILE_MAX_SAMPLES 65536

// How often the trigger is looked for when inotify cannot be used -- which is
// this kernel: CONFIG_INOTIFY_USER is off in it (see the thread below). One
// stat of a name, not an open of the card.
#define PROFILER_POLL_S 5
// A safety net on the event path: a missed event costs one look, not a
// profiler that never runs. A watch that is *gone* is caught by the read error
// instead, and drops the thread to the polling path above.
#define PROFILER_WATCH_TIMEOUT_S 300

typedef struct {
	uintptr_t pc;
	int tid;
} sample_t;

static char trigger_path[600], out_path[600];
static sample_t *samples;
static volatile int sample_count;
static volatile int sampling;
static void (*notify)(bool started);

void profiler_set_notify(void (*cb)(bool started)) { notify = cb; }

static void on_sigprof(int sig, siginfo_t *info, void *context) {
	(void)sig;
	(void)info;
	if (!sampling) {
		return;
	}
	int k = __sync_fetch_and_add(&sample_count, 1);
	if (k >= PROFILE_MAX_SAMPLES) {
		return;
	}
	ucontext_t *uc = context;
#if defined(__mips__)
	samples[k].pc = (uintptr_t)uc->uc_mcontext.pc;
#elif defined(__x86_64__)
	samples[k].pc = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
#else
	samples[k].pc = 0;
#endif
	samples[k].tid = (int)syscall(SYS_gettid);
}

static void copy_file(FILE *out, const char *path) {
	FILE *in = fopen(path, "r");
	if (!in) {
		return;
	}
	char buf[4096];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
		fwrite(buf, 1, n, out);
	}
	fclose(in);
}

static void write_report(void) {
	FILE *f = fopen(out_path, "w");
	if (!f) {
		return;
	}
	fprintf(f, "[maps]\n");
	copy_file(f, "/proc/self/maps");
	fprintf(f, "[threads]\n");
	DIR *d = opendir("/proc/self/task");
	struct dirent *de;
	while (d && (de = readdir(d)) != NULL) {
		if (de->d_name[0] == '.') {
			continue;
		}
		char path[300], name[64] = "";
		snprintf(path, sizeof(path), "/proc/self/task/%s/comm", de->d_name);
		FILE *c = fopen(path, "r");
		if (c) {
			if (fgets(name, sizeof(name), c)) {
				name[strcspn(name, "\n")] = '\0';
			}
			fclose(c);
		}
		fprintf(f, "%s %s\n", de->d_name, name);
	}
	if (d) {
		closedir(d);
	}
	fprintf(f, "[samples]\n");
	int n = sample_count < PROFILE_MAX_SAMPLES ? sample_count : PROFILE_MAX_SAMPLES;
	for (int i = 0; i < n; i++) {
		fprintf(f, "%lx %d\n", (unsigned long)samples[i].pc, samples[i].tid);
	}
	// On the card before anything else: the next thing after "done" is often
	// the cable going in, and the card leaving for the computer with the
	// report still in the page cache came back as an empty file.
	fflush(f);
	fsync(fileno(f));
	fclose(f);
	printf("profiler: %d samples written to %s\n", n, out_path);
}

static void *profiler_thread(void *arg) {
	(void)arg;
	// A run cut short by a reboot left its trigger renamed: arm it again.
	char left[sizeof(trigger_path) + 8];
	snprintf(left, sizeof(left), "%s.armed", trigger_path);
	if (access(left, F_OK) == 0 && access(trigger_path, F_OK) != 0) {
		rename(left, trigger_path);
	}

	// Watching for the trigger instead of asking the card for it every two
	// seconds: the old loop ran one open() on the card half a hertz forever,
	// with the screen off and the device supposed to be idle, for a file that
	// only ever appears when someone is profiling this very build. inotify on
	// the directory the trigger lives in costs nothing until it is written.
	//
	// **Not on this kernel, though.** Measured on the V3 Analog (2026-10-25):
	// inotify_init() and inotify_init1() both answer ENOSYS (the vendor kernel
	// is built without CONFIG_INOTIFY_USER -- /proc/sys/fs/inotify does not
	// exist), so what actually runs here is the fallback below. It is a stat of
	// one name every five seconds, not an open: a missing name is usually a
	// dentry lookup that vfat answers from cache, where an open reads the
	// directory entry and then the file. The event path stays for the builds
	// that do have inotify, and it has to be non-blocking for a reason of its
	// own: the descriptor is drained in a loop, and a blocking read returns the
	// queued events and then waits for the next one, so the loop would never
	// come back to look at the trigger it was woken for. The drain ends on
	// EAGAIN, which is the "nothing more queued" answer.
	int ifd = -1;
	char dir[600];
	snprintf(dir, sizeof(dir), "%s", trigger_path);
	char *slash = strrchr(dir, '/');
	if (slash) {
		*slash = '\0';
		ifd = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
		if (ifd >= 0 && inotify_add_watch(ifd, dir, IN_CREATE | IN_MOVED_TO | IN_CLOSE_WRITE) < 0) {
			close(ifd);
			ifd = -1;
		}
	}

	// No sleep before the first look: the trigger may already be there -- the
	// rename above is one way it gets there -- and a watch only reports changes
	// that happen after it is armed. It goes first either way, so a device left
	// with a trigger on the card starts profiling at boot rather than up to a
	// watch timeout later.
	bool look = true;
	for (;;) {
		if (!look) {
			if (ifd >= 0) {
				// No wake-up at all until the directory changes, with a long
				// timeout that only re-confirms the watch is alive.
				struct pollfd p = {.fd = ifd, .events = POLLIN};
				if (poll(&p, 1, PROFILER_WATCH_TIMEOUT_S * 1000) == 0) {
					continue; // nothing happened; the file is still not there
				}
				char events[1024];
				for (;;) {
					ssize_t got = read(ifd, events, sizeof(events));
					if (got > 0) {
						continue; // drained: one look below covers them all
					}
					if (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
						break;
					}
					if (got < 0 && errno == EINTR) {
						continue;
					}
					// The watch is gone (the card was remounted, or the
					// descriptor broke): fall back to asking the card, which is
					// cheap at this interval and cannot fail this way.
					close(ifd);
					ifd = -1;
					break;
				}
			} else {
				// The path that runs on this device (no inotify): one stat of
				// the trigger's name, five seconds apart, and no open until it
				// is there.
				sleep(PROFILER_POLL_S);
				if (access(trigger_path, F_OK) != 0) {
					continue;
				}
			}
		}
		look = false;
		FILE *t = fopen(trigger_path, "r");
		if (!t) {
			continue;
		}
		int seconds = 0, delay = 0;
		int got = fscanf(t, "%d %d", &seconds, &delay);
		if (got < 1 || seconds <= 0 || seconds > 300) {
			seconds = 20;
		}
		if (got < 2 || delay < 0 || delay > 600) {
			delay = 0;
		}
		fclose(t);
		// The trigger stays until the report is written: a firmware update
		// boots once to install and again to run, and a trigger taken by the
		// first boot was lost with it. Renamed meanwhile, so it is not read twice.
		char armed[sizeof(trigger_path) + 8];
		snprintf(armed, sizeof(armed), "%s.armed", trigger_path);
		rename(trigger_path, armed);
		if (delay) {
			printf("profiler: starting in %d s\n", delay);
			sleep((unsigned)delay);
		}

		if (!samples) {
			samples = malloc(sizeof(*samples) * PROFILE_MAX_SAMPLES);
			if (!samples) {
				continue;
			}
		}
		struct sigaction sa;
		memset(&sa, 0, sizeof(sa));
		sa.sa_sigaction = on_sigprof;
		sa.sa_flags = SA_SIGINFO | SA_RESTART;
		sigaction(SIGPROF, &sa, NULL);

		printf("profiler: sampling for %d s\n", seconds);
		sample_count = 0;
		sampling = 1;
		if (notify) {
			notify(true);
		}
		struct itimerval on = {{0, 1000}, {0, 1000}};
		setitimer(ITIMER_PROF, &on, NULL);
		sleep((unsigned)seconds);
		struct itimerval off = {{0, 0}, {0, 0}};
		setitimer(ITIMER_PROF, &off, NULL);
		sampling = 0;
		write_report();
		unlink(armed);
		if (notify) {
			notify(false);
		}
	}
	return NULL;
}

void profiler_start(const char *sd_root) {
	if (!sd_root || !sd_root[0]) {
		return;
	}
	snprintf(trigger_path, sizeof(trigger_path), "%s/.local/profile", sd_root);
	snprintf(out_path, sizeof(out_path), "%s/.local/profile.out", sd_root);
	pthread_t thread;
	if (pthread_create(&thread, NULL, profiler_thread, NULL) == 0) {
		pthread_detach(thread);
	}
}
