#pragma once

#include "lvgl/lvgl.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

// Writes that leave a widget as it is, without costing what a change does.
//
// LVGL compares nothing: lv_label_set_text() with the words the label already
// shows frees and allocates the text, throws away its size cache and
// invalidates it, and lv_image_set_src() with the source the image already
// has looks the image up again and invalidates it. A poll that refreshes what
// is on screen every half second therefore repainted -- or, with the page
// out of sight, re-laid-out -- widgets that had not changed, and a repaint
// anywhere is a whole refresh of the interface. These skip the write when
// there is nothing to write.

static inline void gui_label_set_text(lv_obj_t *label, const char *text) {
	const char *shown = lv_label_get_text(label);
	if (!shown || strcmp(shown, text) != 0) {
		lv_label_set_text(label, text);
	}
}

static inline void gui_label_set_text_fmt(lv_obj_t *label, const char *fmt, ...) {
	char text[128];
	va_list args;
	va_start(args, fmt);
	int n = vsnprintf(text, sizeof(text), fmt, args);
	va_end(args);
	if (n < 0 || n >= (int)sizeof(text)) {
		va_start(args, fmt);
		lv_label_set_text_vfmt(label, fmt, args); // too long for here: as it always was
		va_end(args);
		return;
	}
	gui_label_set_text(label, text);
}

// For the constant icon descriptors, which are compared by address.
static inline void gui_image_set_src(lv_obj_t *img, const void *src) {
	if (lv_image_get_src(img) != src) {
		lv_image_set_src(img, src);
	}
}
