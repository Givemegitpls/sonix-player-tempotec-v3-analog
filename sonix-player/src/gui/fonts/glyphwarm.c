#include "glyphwarm.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "lvgl/src/font/lv_font_private.h"
#include "lvgl/src/misc/lv_text_private.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/gui.h"
#include "src/system/core/config.h"
#include "src/system/core/utils.h"
#include "src/system/library/library.h"

uint64_t glyphwarm_now_us(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

void glyphwarm_letter(const lv_font_t *font, uint32_t letter) {
	lv_font_glyph_dsc_t g;
	if (!lv_font_get_glyph_dsc(font, &g, letter, 0) || g.is_placeholder || g.box_w == 0 || g.box_h == 0) {
		return;
	}
	if (lv_font_get_glyph_bitmap_internal(&g, NULL)) {
		lv_font_glyph_release_draw_data_internal(&g);
	}
}

bool glyphwarm_text(const lv_font_t *font, const char *text, uint32_t *pos, uint32_t budget_us) {
	if (!font || !text) {
		return true;
	}
	uint64_t begin = glyphwarm_now_us();
	while (text[*pos] != '\0') {
		uint32_t letter = lv_text_encoded_next(text, pos);
		if (letter > ' ') {
			glyphwarm_letter(font, letter);
		}
		if (glyphwarm_now_us() - begin >= budget_us) {
			return text[*pos] == '\0';
		}
	}
	return true;
}

// ---------------------------------------------------------------------------
// The library's own letters
// ---------------------------------------------------------------------------

#define WARM_MAX_LETTERS 320	 // per size: 640 glyphs and bitmaps at most, a few hundred KB
#define CENSUS_SLOTS 16384		 // distinct non-ASCII letters it can tell apart (power of two)
#define CENSUS_MAX_TITLES 20000	 // rows of track titles read; albums and artists are few
#define WARM_TICK_MS 40
#define WARM_SLICE_US 5000
#define WARM_QUIET_MS 300

typedef struct {
	uint32_t letter;
	uint32_t count;
} census_slot_t;

typedef struct {
	census_slot_t *slots;
	uint32_t used;
} census_t;

// The next letter of a UTF-8 string, without touching LVGL (this runs on a
// background thread). Malformed bytes are skipped as single letters.
static uint32_t utf8_next(const unsigned char **p) {
	const unsigned char *s = *p;
	uint32_t c = *s;
	int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
	if (extra == 0) {
		*p = s + 1;
		return c < 0x80 ? c : 0xFFFD;
	}
	c &= (0x3Fu >> extra);
	for (int i = 1; i <= extra; i++) {
		if ((s[i] & 0xC0) != 0x80) {
			*p = s + i;
			return 0xFFFD;
		}
		c = (c << 6) | (s[i] & 0x3F);
	}
	*p = s + extra + 1;
	return c;
}

static bool census_text(const char *text, void *user) {
	census_t *census = user;
	const unsigned char *p = (const unsigned char *)text;
	while (*p) {
		uint32_t letter = utf8_next(&p);
		if (letter < 0x80 || letter == 0xFFFD || (letter >= 0x80 && letter < 0xA0)) {
			continue;
		}
		uint32_t at = (letter * 2654435761u) & (CENSUS_SLOTS - 1);
		for (uint32_t probe = 0; probe < CENSUS_SLOTS; probe++, at = (at + 1) & (CENSUS_SLOTS - 1)) {
			census_slot_t *slot = &census->slots[at];
			if (slot->count == 0) {
				slot->letter = letter;
				slot->count = 1;
				census->used++;
				break;
			}
			if (slot->letter == letter) {
				slot->count++;
				break;
			}
		}
	}
	return true;
}

static int by_count(const void *a, const void *b) {
	const census_slot_t *x = a, *y = b;
	return x->count < y->count ? 1 : x->count > y->count ? -1 : (x->letter > y->letter) - (x->letter < y->letter);
}

typedef struct {
	uint32_t count;
	uint32_t letters[WARM_MAX_LETTERS];
} letter_list_t;

// --- interface thread: warming the list it was handed ----------------------

static letter_list_t *warm_list;
static int warm_font; // 0 the row title's size, 1 the artist's
static uint32_t warm_at;
static lv_timer_t *warm_timer;

static void warm_tick(lv_timer_t *timer) {
	if (!warm_list || lv_display_get_inactive_time(NULL) < WARM_QUIET_MS) {
		return;
	}
	const lv_font_t *fonts[2] = {&font_ui_24, &font_ui_20};
	uint64_t begin = glyphwarm_now_us();
	while (glyphwarm_now_us() - begin < WARM_SLICE_US) {
		if (warm_font >= 2) {
			lv_timer_pause(timer);
			return;
		}
		if (warm_at >= warm_list->count) {
			warm_at = 0;
			warm_font++;
			continue;
		}
		glyphwarm_letter(fonts[warm_font], warm_list->letters[warm_at++]);
	}
}

static void warm_adopt_cb(void *user) {
	letter_list_t *list = user;
	free(warm_list);
	warm_list = list;
	warm_font = 0;
	warm_at = 0;
	if (list->count == 0) {
		if (warm_timer) {
			lv_timer_pause(warm_timer);
		}
		return;
	}
	if (!warm_timer) {
		warm_timer = lv_timer_create(warm_tick, WARM_TICK_MS, NULL);
	}
	lv_timer_resume(warm_timer);
}

// --- the census thread ------------------------------------------------------

static pthread_mutex_t census_lock = PTHREAD_MUTEX_INITIALIZER;
static bool census_running;
static bool census_again; // asked for again while it was running

static void *census_thread(void *arg) {
	(void)arg;
	thread_be_background("glyphcensus");

	for (;;) {
		census_t census = {.slots = calloc(CENSUS_SLOTS, sizeof(census_slot_t)), .used = 0};
		letter_list_t *list = calloc(1, sizeof(*list));
		if (census.slots && list) {
			library_names_walk(census_text, &census, CENSUS_MAX_TITLES);

			// Compacted and sorted by count, most used first.
			uint32_t n = 0;
			for (uint32_t i = 0; i < CENSUS_SLOTS; i++) {
				if (census.slots[i].count) {
					census.slots[n++] = census.slots[i];
				}
			}
			qsort(census.slots, n, sizeof(census.slots[0]), by_count);
			list->count = n < WARM_MAX_LETTERS ? n : WARM_MAX_LETTERS;
			for (uint32_t i = 0; i < list->count; i++) {
				list->letters[i] = census.slots[i].letter;
			}
			fprintf(stderr, "glyphwarm: %u distinct non-ASCII letters in the library, warming the %u most used\n",
					(unsigned)n, (unsigned)list->count);
			free(census.slots);
			if (!gui_post(warm_adopt_cb, list)) {
				free(list);
			}
		} else {
			free(census.slots);
			free(list);
		}

		pthread_mutex_lock(&census_lock);
		if (!census_again) {
			census_running = false;
			pthread_mutex_unlock(&census_lock);
			return NULL;
		}
		census_again = false;
		pthread_mutex_unlock(&census_lock);
	}
}

void glyphwarm_library_refresh(void) {
	if (!config_get_bool("ui", "warm_glyphs", true) || !library_is_open()) {
		return;
	}
	pthread_mutex_lock(&census_lock);
	if (census_running) {
		census_again = true;
		pthread_mutex_unlock(&census_lock);
		return;
	}
	census_running = true;
	pthread_mutex_unlock(&census_lock);

	pthread_t thread;
	if (pthread_create(&thread, NULL, census_thread, NULL) == 0) {
		pthread_detach(thread);
	} else {
		pthread_mutex_lock(&census_lock);
		census_running = false;
		pthread_mutex_unlock(&census_lock);
	}
}
