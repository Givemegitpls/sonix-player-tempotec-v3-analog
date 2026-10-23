#include "spinner.h"

#include "src/gui/shell/animpark.h"
#include "src/gui/shell/theme.h"

// One full turn. A second and a half: faster looks frantic, slower looks stalled.
#define SPIN_PERIOD_MS 1500

// LVGL angles are in tenths of a degree, so a full turn is 3600.
#define SPIN_FULL_TURN 3600

// Only while it can be seen. A spinner hidden with its page or its busy layer
// -- the Qobuz, Tidal and podcast layers are built at boot and hidden --
// turned on regardless, forever: every turn step forced a layout of its screen
// and kept LVGL's animation timer, and with it the interface, awake every
// frame with nothing on screen moving. A turn step that finds the spinner out
// of sight stops it; the start of the refresh that brings it back into sight
// starts it again (spin_refr_start_cb).
#define SPINNERS_MAX 16
static lv_obj_t *spinners[SPINNERS_MAX];

static bool spinner_shown(lv_obj_t *img) {
	lv_obj_t *screen = lv_obj_get_screen(img);
	if (screen != lv_screen_active() && screen != lv_layer_top() && screen != lv_layer_sys()) {
		return false;
	}
	for (lv_obj_t *o = img; o; o = lv_obj_get_parent(o)) {
		if (lv_obj_is_hidden(o)) {
			return false;
		}
	}
	return true;
}

static bool spinner_kept(lv_obj_t *img) {
	for (int i = 0; i < SPINNERS_MAX; i++) {
		if (spinners[i] == img) {
			return true;
		}
	}
	return false;
}

static void spin_start(lv_obj_t *img);

static void spin_cb(void *obj, int32_t angle) {
	// Only one the table holds is stopped: nothing would start any other again.
	if (!spinner_shown((lv_obj_t *)obj) && spinner_kept((lv_obj_t *)obj)) {
		lv_anim_delete(obj, spin_cb);
		return;
	}
	lv_image_set_rotation((lv_obj_t *)obj, angle);
}

static void spin_refr_start_cb(lv_event_t *e) {
	(void)e;
	for (int i = 0; i < SPINNERS_MAX; i++) {
		if (spinners[i] && !lv_anim_get(spinners[i], spin_cb) && spinner_shown(spinners[i])) {
			spin_start(spinners[i]);
		}
	}
}

static void spin_deleted_cb(lv_event_t *e) {
	lv_obj_t *img = lv_event_get_target(e);
	for (int i = 0; i < SPINNERS_MAX; i++) {
		if (spinners[i] == img) {
			spinners[i] = NULL;
		}
	}
}

// One endless turn from wherever the icon stands, so a spinner given back by
// animpark carries on from where it stopped.
static void spin_start(lv_obj_t *img) {
	int32_t from = lv_image_get_rotation(img);
	lv_anim_t a;
	lv_anim_init(&a);
	lv_anim_set_var(&a, img);
	lv_anim_set_exec_cb(&a, spin_cb);
	lv_anim_set_values(&a, from, from + SPIN_FULL_TURN);
	lv_anim_set_duration(&a, SPIN_PERIOD_MS);
	lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
	// Linear: a spinner that speeds up and slows down each turn looks broken.
	lv_anim_set_path_cb(&a, lv_anim_path_linear);
	lv_anim_start(&a);
}

static void spin_stop(lv_obj_t *img) { lv_anim_delete(img, spin_cb); }

lv_obj_t *spinner_create(lv_obj_t *parent, const lv_image_dsc_t *icon) {
	lv_obj_t *img = lv_image_create(parent);
	lv_image_set_src(img, icon);
	lv_obj_add_style(img, &theme_style_icon, 0);
	lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);

	// Pivot at the image centre; otherwise it spins around the top-left corner
	// and appears to fly off.
	lv_image_set_pivot(img, (int32_t)icon->header.w / 2, (int32_t)icon->header.h / 2);

	// Rotation pushes pixels outside the image rectangle: without this LVGL
	// draws within the original bounds and clips the circle's corners every
	// quarter turn.
	lv_obj_set_overflow_visible(img, true);

	static bool refr_hooked;
	if (!refr_hooked) {
		refr_hooked = true;
		lv_display_add_event_cb(lv_display_get_default(), spin_refr_start_cb, LV_EVENT_REFR_START, NULL);
	}
	spin_start(img);
	// Spinners wait on hidden layers and on pages left behind; turning there
	// would keep LVGL's animation timer awake for nothing.
	animpark_watch(img, spin_stop, spin_start);
	bool kept = false;
	for (int i = 0; i < SPINNERS_MAX && !kept; i++) {
		if (!spinners[i]) {
			spinners[i] = img;
			kept = true;
		}
	}
	lv_obj_add_event_cb(img, spin_deleted_cb, LV_EVENT_DELETE, NULL);

	// Turning from the start; a spinner built out of sight stops at its first
	// step. One that found no slot in the table turns as before, always.
	spin_start(img);
	return img;
}
