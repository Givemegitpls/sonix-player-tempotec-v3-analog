// Which interface timer a slow pass was spent in.
//
// LVGL's own profiling point around a timer callback says only "timer_cb".
// So every timer the player creates runs through timer_trampoline() instead,
// which knows the real callback and, while frameprof is on, times it: two
// clock reads per callback, against the thousands the drawing points cost.
// The redirection is the linker's (--wrap=lv_timer_create, --wrap=
// lv_timer_set_cb in the Makefile): no LVGL source is touched, and callers
// keep their user data, since the timer itself is unchanged.

#include "src/system/core/frameprof.h"

#include "lvgl/lvgl.h"

#include <stdint.h>
#include <string.h>

lv_timer_t *__real_lv_timer_create(lv_timer_cb_t cb, uint32_t period, void *user_data);
void __real_lv_timer_set_cb(lv_timer_t *timer, lv_timer_cb_t cb);

// Timer -> real callback. Open addressing; a timer freed by LVGL leaves its
// entry, which the next timer at the same address simply overwrites, and when
// the table fills up the entries of timers that no longer exist are dropped.
#define SLOTS 1024

typedef struct {
	lv_timer_t *timer;
	lv_timer_cb_t cb;
} slot_t;

static slot_t slots[SLOTS];
static int used;

static unsigned slot_of(const lv_timer_t *t) { return (unsigned)(((uintptr_t)t >> 3) * 2654435761u) % SLOTS; }

static slot_t *find(const lv_timer_t *t) {
	unsigned h = slot_of(t);
	for (unsigned i = 0; i < SLOTS; i++) {
		slot_t *s = &slots[(h + i) % SLOTS];
		if (s->timer == t) {
			return s;
		}
		if (!s->timer) {
			return NULL;
		}
	}
	return NULL;
}

static bool insert(lv_timer_t *t, lv_timer_cb_t cb) {
	unsigned h = slot_of(t);
	for (unsigned i = 0; i < SLOTS; i++) {
		slot_t *s = &slots[(h + i) % SLOTS];
		if (s->timer == t || !s->timer) {
			if (!s->timer) {
				used++;
			}
			s->timer = t;
			s->cb = cb;
			return true;
		}
	}
	return false;
}

// Keeps only the timers LVGL still has, then rebuilds the table.
static void prune(void) {
	static slot_t keep[SLOTS];
	int n = 0;
	for (lv_timer_t *t = lv_timer_get_next(NULL); t && n < SLOTS; t = lv_timer_get_next(t)) {
		slot_t *s = find(t);
		if (s) {
			keep[n++] = *s;
		}
	}
	memset(slots, 0, sizeof(slots));
	used = 0;
	for (int i = 0; i < n; i++) {
		insert(keep[i].timer, keep[i].cb);
	}
}

static void timer_trampoline(lv_timer_t *timer) {
	slot_t *s = find(timer);
	if (!s || !s->cb) {
		return;
	}
	lv_timer_cb_t cb = s->cb;
	if (!frameprof_timing()) {
		cb(timer);
		return;
	}
	uint64_t t0 = frameprof_now_us();
	cb(timer);
	frameprof_timer_ran((void *)cb, (uint32_t)(frameprof_now_us() - t0));
}

static bool wrap(lv_timer_t *timer, lv_timer_cb_t cb) {
	if (used > SLOTS * 3 / 4) {
		prune();
	}
	return insert(timer, cb);
}

lv_timer_t *__wrap_lv_timer_create(lv_timer_cb_t cb, uint32_t period, void *user_data) {
	lv_timer_t *timer = __real_lv_timer_create(cb, period, user_data);
	if (timer && cb && wrap(timer, cb)) {
		__real_lv_timer_set_cb(timer, timer_trampoline);
	}
	return timer;
}

void __wrap_lv_timer_set_cb(lv_timer_t *timer, lv_timer_cb_t cb) {
	if (timer && cb && wrap(timer, cb)) {
		__real_lv_timer_set_cb(timer, timer_trampoline);
		return;
	}
	__real_lv_timer_set_cb(timer, cb); // no room: unwatched, but working
}
