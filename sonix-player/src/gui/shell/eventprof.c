// Which event callback a slow pass was spent in -- the same idea as
// timerprof.c, for the handlers LVGL calls on a touch, a scroll or a redraw.
//
// Only in a profiled run (frameprof_wanted()): each callback registered then
// gets a small record holding the real callback and user data, and a
// trampoline that puts the user data back in the event before calling it and
// times the call. Those records are not freed with their objects, which is
// why an ordinary run never makes them. lv_obj_remove_event_cb is wrapped too,
// so removing a callback by its function still finds it.

#include "src/system/core/frameprof.h"

#include "lvgl/lvgl.h"
#include "lvgl/src/misc/lv_event_private.h"

#include <stdlib.h>

lv_event_dsc_t *__real_lv_obj_add_event_cb(lv_obj_t *obj, lv_event_cb_t cb, lv_event_code_t filter,
										   void *user_data);
uint32_t __real_lv_obj_remove_event_cb(lv_obj_t *obj, lv_event_cb_t cb);

typedef struct {
	lv_event_cb_t cb;
	void *user_data;
} wrapped_t;

static void event_trampoline(lv_event_t *e) {
	wrapped_t *w = e->user_data;
	e->user_data = w->user_data;
	if (!frameprof_timing()) {
		w->cb(e);
		return;
	}
	uint64_t t0 = frameprof_now_us();
	w->cb(e);
	frameprof_event_ran((void *)w->cb, (unsigned)(frameprof_now_us() - t0));
}

lv_event_dsc_t *__wrap_lv_obj_add_event_cb(lv_obj_t *obj, lv_event_cb_t cb, lv_event_code_t filter,
										   void *user_data) {
	if (!cb || !frameprof_wanted()) {
		return __real_lv_obj_add_event_cb(obj, cb, filter, user_data);
	}
	wrapped_t *w = malloc(sizeof(*w));
	if (!w) {
		return __real_lv_obj_add_event_cb(obj, cb, filter, user_data);
	}
	w->cb = cb;
	w->user_data = user_data;
	return __real_lv_obj_add_event_cb(obj, event_trampoline, filter, w);
}

uint32_t __wrap_lv_obj_remove_event_cb(lv_obj_t *obj, lv_event_cb_t cb) {
	uint32_t removed = __real_lv_obj_remove_event_cb(obj, cb);
	if (!obj || !frameprof_wanted()) {
		return removed;
	}
	for (int32_t i = (int32_t)lv_obj_get_event_count(obj) - 1; i >= 0; i--) {
		lv_event_dsc_t *dsc = lv_obj_get_event_dsc(obj, (uint32_t)i);
		if (dsc && lv_event_dsc_get_cb(dsc) == event_trampoline) {
			wrapped_t *w = lv_event_dsc_get_user_data(dsc);
			if (w && w->cb == cb) {
				lv_obj_remove_event(obj, (uint32_t)i);
				free(w);
				removed++;
			}
		}
	}
	return removed;
}
