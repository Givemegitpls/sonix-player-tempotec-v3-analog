#pragma once

#include "lvgl/lvgl.h"

#include <stdbool.h>

// Whether `obj` can be seen: on the active screen or one of the layers, with
// nothing between it and there hidden. Coordinates are not asked, since a page
// never shown has not been laid out yet, and neither is whether something
// opaque sits over it: this is the cheap test the animations that only matter
// on screen use to decide if they should be running (spinner.c, scrolltext.c,
// the caret in keyboard.c).
static inline bool gui_obj_shown(lv_obj_t *obj) {
	lv_obj_t *screen = lv_obj_get_screen(obj);
	if (screen != lv_screen_active() && screen != lv_layer_top() && screen != lv_layer_sys()) {
		return false;
	}
	for (lv_obj_t *o = obj; o; o = lv_obj_get_parent(o)) {
		if (lv_obj_is_hidden(o)) {
			return false;
		}
	}
	return true;
}
