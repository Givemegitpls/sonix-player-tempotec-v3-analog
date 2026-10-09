#include "src/system/core/frameprof.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define TAGS 256
#define DEPTH 64
#define SHOWN 8

typedef struct {
	const char *tag;
	uint32_t self_us, total_us, count;
} tag_stat_t;

typedef struct {
	const char *tag;
	uint64_t start_us;
	uint64_t children_us;
} frame_t;

static bool active;
static uint32_t slow_ms = FRAMEPROF_SLOW_MS;
static bool in_pass;
static pthread_t ui_thread;
static tag_stat_t stats[TAGS];
static frame_t stack[DEPTH];
static int depth;
static uint64_t pass_start_us;

#define TIMERS_SHOWN 4

// The slowest timer callbacks of the pass, by address (sonix_player_debug
// names them).
typedef struct {
	void *cb;
	uint32_t us;
} slow_cb_t;

static slow_cb_t slow_timers[TIMERS_SHOWN];
static slow_cb_t slow_events[TIMERS_SHOWN];

static uint64_t now_us(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static tag_stat_t *stat_for(const char *tag) {
	uintptr_t h = ((uintptr_t)tag >> 3) % TAGS;
	for (int i = 0; i < TAGS; i++) {
		tag_stat_t *s = &stats[(h + (uintptr_t)i) % TAGS];
		if (s->tag == tag) {
			return s;
		}
		if (!s->tag) {
			s->tag = tag;
			return s;
		}
	}
	return NULL; // full: this pass loses the tag
}

void frameprof_init(bool enabled, unsigned threshold_ms) {
	ui_thread = pthread_self();
	active = enabled;
	if (threshold_ms) {
		slow_ms = threshold_ms;
	}
	if (enabled) {
		printf("frameprof: interface passes over %u ms are broken down in the log\n", (unsigned)slow_ms);
	}
}

bool frameprof_timing(void) { return in_pass; }

unsigned long long frameprof_now_us(void) { return now_us(); }

static void note_slow(slow_cb_t *list, void *cb, unsigned us) {
	int slot = -1;
	for (int i = 0; i < TIMERS_SHOWN; i++) {
		if (list[i].cb == cb) {
			list[i].us += us;
			return;
		}
		if (slot < 0 || list[i].us < list[slot].us) {
			slot = i;
		}
	}
	if (us > list[slot].us) {
		list[slot].cb = cb;
		list[slot].us = us;
	}
}

void frameprof_timer_ran(void *cb, unsigned us) { note_slow(slow_timers, cb, us); }
void frameprof_event_ran(void *cb, unsigned us) { note_slow(slow_events, cb, us); }

bool frameprof_wanted(void) {
	static int wanted = -1;
	if (wanted < 0) {
		wanted = getenv("SONIX_FRAMEPROF") || access("/mnt/sd_0/.local/frameprof", F_OK) == 0;
	}
	return wanted;
}

void frameprof_pass_begin(void) {
	if (!active) {
		return;
	}
	memset(stats, 0, sizeof(stats));
	memset(slow_timers, 0, sizeof(slow_timers));
	memset(slow_events, 0, sizeof(slow_events));
	depth = 0;
	in_pass = true;
	pass_start_us = now_us();
}

static int by_self(const void *a, const void *b) {
	const tag_stat_t *x = a, *y = b;
	return x->self_us < y->self_us ? 1 : x->self_us > y->self_us ? -1 : 0;
}

void frameprof_pass_end(void) {
	if (!active || !in_pass) {
		return;
	}
	in_pass = false;
	uint32_t pass_ms = (uint32_t)((now_us() - pass_start_us) / 1000u);
	if (pass_ms < slow_ms) {
		return;
	}
	tag_stat_t top[TAGS];
	int n = 0;
	for (int i = 0; i < TAGS; i++) {
		if (stats[i].tag) {
			top[n++] = stats[i];
		}
	}
	qsort(top, (size_t)n, sizeof(top[0]), by_self);
	char line[1024];
	int used = snprintf(line, sizeof(line), "frameprof: pass %u ms:", pass_ms);
	for (int i = 0; i < n && i < SHOWN && used < (int)sizeof(line) - 80; i++) {
		used += snprintf(line + used, sizeof(line) - (size_t)used, " %s %u.%u/%u.%u ms x%u;", top[i].tag,
						 top[i].self_us / 1000u, top[i].self_us / 100u % 10u, top[i].total_us / 1000u,
						 top[i].total_us / 100u % 10u, top[i].count);
	}
	for (int i = 0; i < TIMERS_SHOWN && used < (int)sizeof(line) - 40; i++) {
		if (slow_timers[i].cb && slow_timers[i].us >= 1000) {
			used += snprintf(line + used, sizeof(line) - (size_t)used, " timer %p %u.%u ms;", slow_timers[i].cb,
							 slow_timers[i].us / 1000u, slow_timers[i].us / 100u % 10u);
		}
	}
	for (int i = 0; i < TIMERS_SHOWN && used < (int)sizeof(line) - 40; i++) {
		if (slow_events[i].cb && slow_events[i].us >= 1000) {
			used += snprintf(line + used, sizeof(line) - (size_t)used, " event %p %u.%u ms;", slow_events[i].cb,
							 slow_events[i].us / 1000u, slow_events[i].us / 100u % 10u);
		}
	}
	printf("%s\n", line);
}

void frameprof_begin(const char *tag) {
	if (!in_pass || depth >= DEPTH || !pthread_equal(pthread_self(), ui_thread)) {
		return;
	}
	stack[depth++] = (frame_t){tag, now_us(), 0};
}

void frameprof_end(const char *tag) {
	if (!in_pass || depth == 0 || !pthread_equal(pthread_self(), ui_thread)) {
		return;
	}
	// A begin without its end (an early return in LVGL) is closed with the
	// one that encloses it.
	int at = depth - 1;
	while (at > 0 && stack[at].tag != tag) {
		at--;
	}
	if (stack[at].tag != tag) {
		return;
	}
	uint64_t t = now_us();
	while (depth > at) {
		frame_t f = stack[--depth];
		uint64_t elapsed = t - f.start_us;
		tag_stat_t *s = stat_for(f.tag);
		if (s) {
			uint64_t self = elapsed > f.children_us ? elapsed - f.children_us : 0;
			s->self_us += (uint32_t)self;
			s->total_us += (uint32_t)elapsed;
			s->count++;
		}
		if (depth > 0) {
			stack[depth - 1].children_us += elapsed;
		}
	}
}
