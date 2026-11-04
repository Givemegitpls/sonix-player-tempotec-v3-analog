#define _GNU_SOURCE // dl_iterate_phdr, and strerror_r's GNU half
#include "addon.h"

#include "addon_notify.h"

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <link.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "src/gui/shell/main_menu.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/gridpage.h"
#include "src/gui/shell/icons_select.h"
#include "src/gui/shell/switcher.h"
#include "src/system/core/json.h"
#include "src/system/streaming/service.h"
#include "src/system/core/lang.h"
#include "src/system/image/stb_image_decl.h"

#include "lvgl/lvgl.h"

// The tile's picture is the size the compiled-in ones are: the same 111 units
// through SC() that assets/icons/the menu tiles are drawn at. The loader scales
// whatever PNG it is given down to it, so an addon can ship a 512x512 icon and
// pay for it once, at scan time.
#define ADDON_ICON_BOX SC(111)

// A manifest is a dozen lines. Past this it is not one, and reading it would
// only be a way to eat the heap.
#define ADDON_MANIFEST_MAX 8192

// The picture an addon gets when it ships none, or ships one that will not
// decode: the menu's own "more" tile, which is a box of squares and reads as
// "something else lives here". An addon that cares ships icon.png.
static const lv_image_dsc_t *fallback_icon(void) { return &icon_menu_more; }

// A file on the card, as the loader last saw it. Used to answer one question:
// is the library we have open still the library that is on the card? An addon
// updated over adb, over the network or by a card swap is a different file --
// different inode, size or mtime -- and the open copy is then stale code that
// has to go before the new one can be opened.
typedef struct {
	bool valid;
	dev_t dev;
	ino_t ino;
	off_t size;
	time_t mtime;
} file_id_t;

static bool file_id_read(const char *path, file_id_t *out) {
	struct stat st;
	if (stat(path, &st) != 0) {
		memset(out, 0, sizeof(*out));
		return false;
	}
	out->valid = true;
	out->dev = st.st_dev;
	out->ino = st.st_ino;
	out->size = st.st_size;
	out->mtime = st.st_mtime;
	return true;
}

static bool file_id_equal(const file_id_t *a, const file_id_t *b) {
	if (!a->valid || !b->valid) {
		return false;
	}
	// FAT stores mtime to the second and copies in place can keep the size, but
	// a replaced file is a new inode in every path the player has: adb push and
	// a card reader both write a new cluster chain.
	return a->dev == b->dev && a->ino == b->ino && a->size == b->size && a->mtime == b->mtime;
}

typedef struct {
	bool present;
	char id[ADDON_ID_MAX];
	char dir[ADDON_PATH_MAX];
	char title[ADDON_TITLE_MAX];
	char version[16];
	addon_section_t section;
	int order; // the manifest's `order`, 0 when it does not ask for a place

	// What the two files looked like when this slot was read and, if it is open,
	// when the library was dlopen()ed.
	file_id_t manifest_id;
	file_id_t so_id;

	// The service declaration (addon_service.h): the paths it owns on the card,
	// kept here because the player asks about them before this library is ever
	// opened.
	bool is_service;
	const char *owns_paths[SERVICE_PREFIX_MAX];
	int owns_count;
	char owns_path[SERVICE_PREFIX_MAX * 96];
	char cache_path[128];

	// Where the tile's picture comes from: decoded into ARGB8888 of our own,
	// or the compiled-in fallback. `icon` is what the caller hands to
	// lv_image_set_src().
	lv_image_dsc_t art;
	const lv_image_dsc_t *icon;

	// The library, once the tile has been tapped.
	bool loaded;
	void *handle;
	const sonix_addon_t *lib;
	sonix_addon_t lib_copy; // the descriptor as the .so handed it over
	const sonix_addon_stop_t *stop_lib; // likewise, and the way out
	bool (*stop)(void);
	lv_obj_t *screen; // what init() returned
	// Every screen this addon put on a display, init()'s included: an addon that
	// opens its own sub-pages registers them (sonix_addon_own_screen), because a
	// page nobody remembers is a page whose widget callbacks outlive the library.
	lv_obj_t *screens[ADDON_SCREENS_MAX];
	int screen_count;
	// stop() said a thread is still in the library: the mapping stays and the
	// tile will not be opened again this run.
	bool unload_refused;
} addon_slot_t;

static addon_slot_t slots[ADDON_MAX];
static int slot_count;
static addon_info_t info[ADDON_MAX]; // what addon_get() hands out
static grid_action_t tile_actions[ADDON_MAX]; // lives as long as the grids do
static sonix_addon_host_t host;

// What the folder was scanned with, so that a card arriving later can be read
// without asking the shell for the config again.
static gui_config_t scanned_cfg;
static char scanned_root[ADDON_PATH_MAX];
static void (*refresh_hook)(void);

void addon_set_refresh_hook(void (*hook)(void)) { refresh_hook = hook; }

// ---------------------------------------------------------------------------
// Small file helpers
// ---------------------------------------------------------------------------

static bool read_small_file(const char *path, char *out, size_t size, long *out_len) {
	FILE *f = fopen(path, "rb");
	if (!f) {
		return false;
	}
	size_t n = fread(out, 1, size - 1, f);
	bool bad = ferror(f) != 0;
	fclose(f);
	if (bad) {
		return false;
	}
	out[n] = '\0';
	if (out_len) {
		*out_len = (long)n;
	}
	return true;
}

static bool file_exists(const char *path) {
	struct stat st;
	return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

// ---------------------------------------------------------------------------
// The manifest
//
//   {
//     "api": 1,
//     "title": "Example",
//     "titles": {"Russian": "Пример"},
//     "section": "streaming",          // or "more"
//     "order": 2,                      // optional: the tile's place on the page
//     "icon": "icon.png",              // optional; icon.png is tried anyway
//     "version": "0.1"                 // optional, shown in the log
//   }
//
// `id` is the directory name and is not read: two names for one addon would
// only be a way to disagree with each other.
//
// `order` is what lets an addon take the place a compiled-in tile used to have,
// so that moving a page out of the binary does not move the tile: it is the
// 1-based position in its section's grid, and gridpage_set_entries() puts it
// there (see gridpage.h). Without it the tile goes after the built-in ones.
// ---------------------------------------------------------------------------

static bool parse_section(const char *name, addon_section_t *out) {
	if (strcmp(name, "streaming") == 0) {
		*out = ADDON_SECTION_STREAMING;
		return true;
	}
	if (strcmp(name, "more") == 0) {
		*out = ADDON_SECTION_MORE;
		return true;
	}
	return false;
}

static bool manifest_read(const char *path, addon_slot_t *slot) {
	char text[ADDON_MANIFEST_MAX];
	if (!read_small_file(path, text, sizeof(text), NULL)) {
		fprintf(stderr, "addon: %s is unreadable\n", path);
		return false;
	}

	json_doc_t doc;
	if (!json_parse(text, &doc)) {
		fprintf(stderr, "addon: %s is not valid JSON\n", path);
		return false;
	}

	int root = json_root(&doc);
	bool ok = false;
	long api = json_obj_long(&doc, root, "api", -1);

	char raw_title[ADDON_TITLE_MAX] = "";
	char raw_section[24] = "";
	if (!json_obj_str(&doc, root, "title", raw_title, sizeof(raw_title)) || !raw_title[0]) {
		fprintf(stderr, "addon: %s has no title\n", path);
		goto out;
	}
	if (!json_obj_str(&doc, root, "section", raw_section, sizeof(raw_section)) ||
		!parse_section(raw_section, &slot->section)) {
		fprintf(stderr, "addon: %s asks for section '%s', which does not exist\n", path, raw_section);
		goto out;
	}

	// The addon's own translations first: the interface's language files are
	// part of the rootfs and cannot know about an app that arrived on a card.
	int titles = json_get(&doc, root, "titles");
	char translated[ADDON_TITLE_MAX] = "";
	if (titles >= 0 && json_obj_str(&doc, titles, lang_current(), translated, sizeof(translated)) &&
		translated[0]) {
		snprintf(slot->title, sizeof(slot->title), "%s", translated);
	} else {
		snprintf(slot->title, sizeof(slot->title), "%s", raw_title);
	}

	json_obj_str(&doc, root, "version", slot->version, sizeof(slot->version));

	// The place on the page, when the addon asks for one. A number the grid
	// cannot use is not worth an error: the tile simply goes after the others.
	long order = json_obj_long(&doc, root, "order", 0);
	slot->order = (order > 0 && order < 100) ? (int)order : 0;

	// The service half: where it keeps its files, and whether it is one at all.
	// Kept as pointers into the parsed document until manifest_read() is done --
	// service_register_paths() is called from slot_read() with the strings.
	slot->is_service = json_obj_bool(&doc, root, "service", false);
	int owns = json_get(&doc, root, "owns_paths");
	slot->owns_count = 0;
	for (int i = 0; owns >= 0 && i < SERVICE_PREFIX_MAX; i++) {
		int element = json_at(&doc, owns, i);
		char prefix[96] = "";
		if (element < 0 || !json_str(&doc, element, prefix, sizeof(prefix)) || !prefix[0]) {
			break;
		}
		snprintf(slot->owns_path + i * sizeof(prefix), sizeof(prefix), "%s", prefix);
		slot->owns_paths[slot->owns_count++] = slot->owns_path + i * sizeof(prefix);
	}
	json_obj_str(&doc, root, "cache_path", slot->cache_path, sizeof(slot->cache_path));

	if (api != SONIX_ADDON_API) {
		// Not a reason to hide the tile: the sentence the user gets when they
		// tap it says what is wrong, and it says it about a tile they can see.
		fprintf(stderr, "addon: %s/%s speaks API %ld, this player speaks %d\n", slot->id, path, api,
				SONIX_ADDON_API);
	}
	ok = true;

out:
	json_free(&doc);
	return ok;
}

// ---------------------------------------------------------------------------
// The tile's picture
//
// Decoded here rather than through the player's cover loader, and in ARGB8888
// rather than RGB565, because of one thing: alpha. The cover path asks
// stb_image for three channels (a cover is a photograph, the fourth is noise)
// and a logo drawn on transparency comes out of that with black in the
// transparent pixels -- a black square behind a white glyph, which is exactly
// what the Subsonic tile looked like. Four channels and an image descriptor
// LVGL blends is the whole fix.
//
// The scaling is an area average with the colour weighted by alpha: averaging
// colour and alpha separately makes a transparent edge pixel drag the colour
// towards black, which is a dark fringe around every glyph at tile size.
// ---------------------------------------------------------------------------

#define ADDON_ICON_MAX_SOURCE_PIXELS (4096 * 4096)

typedef struct {
	uint8_t r, g, b, a;
} rgba_t;

static bool icon_pack(const uint8_t *rgba, int sw, int sh, int box, lv_image_dsc_t *dsc) {
	if (sw < 1 || sh < 1 || box < 1) {
		return false;
	}

	double scale = (double)box / (double)(sw > sh ? sw : sh);
	if (scale > 1.0) {
		scale = 1.0; // never upscale: a 16x16 icon stays 16x16 and sharp
	}
	int dw = (int)((double)sw * scale + 0.5);
	int dh = (int)((double)sh * scale + 0.5);
	if (dw < 1) {
		dw = 1;
	}
	if (dh < 1) {
		dh = 1;
	}

	uint8_t *out = calloc((size_t)dw * (size_t)dh, 4);
	if (!out) {
		return false;
	}

	for (int y = 0; y < dh; y++) {
		int y0 = (int)((double)y * sh / dh);
		int y1 = (int)((double)(y + 1) * sh / dh);
		if (y1 <= y0) {
			y1 = y0 + 1;
		}
		for (int x = 0; x < dw; x++) {
			int x0 = (int)((double)x * sw / dw);
			int x1 = (int)((double)(x + 1) * sw / dw);
			if (x1 <= x0) {
				x1 = x0 + 1;
			}

			// Premultiplied sums, so a fully transparent pixel contributes
			// nothing to the colour it is averaged with.
			double r = 0, g = 0, b = 0, a = 0;
			int n = 0;
			for (int sy = y0; sy < y1 && sy < sh; sy++) {
				for (int sx = x0; sx < x1 && sx < sw; sx++) {
					const uint8_t *px = rgba + ((size_t)sy * sw + sx) * 4;
					double pa = px[3] / 255.0;
					r += px[0] * pa;
					g += px[1] * pa;
					b += px[2] * pa;
					a += pa;
					n++;
				}
			}
			if (n == 0) {
				continue;
			}
			double wa = a / n;
			uint8_t alpha = (uint8_t)(wa * 255.0 + 0.5);
			uint8_t *dst = out + ((size_t)y * dw + x) * 4;
			// LVGL's ARGB8888 is B, G, R, A in memory (lv_color32_t).
			dst[3] = alpha;
			if (a > 0.0) {
				dst[0] = (uint8_t)(b / a + 0.5);
				dst[1] = (uint8_t)(g / a + 0.5);
				dst[2] = (uint8_t)(r / a + 0.5);
			}
		}
	}

	memset(dsc, 0, sizeof(*dsc));
	dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
	dsc->header.cf = LV_COLOR_FORMAT_ARGB8888;
	dsc->header.w = (uint32_t)dw;
	dsc->header.h = (uint32_t)dh;
	dsc->header.stride = (uint32_t)dw * 4;
	dsc->data_size = (uint32_t)dw * dh * 4;
	dsc->data = out;
	return true;
}

static void icon_load(addon_slot_t *slot, const char *name) {
	slot->icon = fallback_icon();
	if (!name || !name[0]) {
		return;
	}

	char path[ADDON_PATH_MAX];
	snprintf(path, sizeof(path), "%s/%s", slot->dir, name);

	FILE *f = fopen(path, "rb");
	if (!f) {
		return;
	}
	if (fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		return;
	}
	long len = ftell(f);
	// An icon that is a megabyte is somebody's mistake, not a tile.
	if (len <= 0 || len > (long)(4 * 1024 * 1024) || fseek(f, 0, SEEK_SET) != 0) {
		fclose(f);
		return;
	}
	uint8_t *data = malloc((size_t)len);
	if (!data) {
		fclose(f);
		return;
	}
	bool ok = fread(data, 1, (size_t)len, f) == (size_t)len;
	fclose(f);

	int w = 0, h = 0, channels = 0;
	uint8_t *rgba = NULL;
	if (ok) {
		rgba = stbi_load_from_memory(data, (int)len, &w, &h, &channels, 4);
	}
	free(data);

	if (!rgba) {
		fprintf(stderr, "addon: %s/%s is not a picture this player decodes\n", slot->id, name);
		return;
	}
	if ((long long)w * h > ADDON_ICON_MAX_SOURCE_PIXELS) {
		fprintf(stderr, "addon: %s/%s is %dx%d, too large for a tile\n", slot->id, name, w, h);
		stbi_image_free(rgba);
		return;
	}

	if (!icon_pack(rgba, w, h, ADDON_ICON_BOX, &slot->art)) {
		fprintf(stderr, "addon: %s/%s could not be scaled\n", slot->id, name);
	}
	stbi_image_free(rgba);

	if (slot->art.data) {
		slot->icon = &slot->art;
	}
}

// ---------------------------------------------------------------------------
// Stopping one: the way a library leaves the process
//
// Everything here runs on the GUI thread and in one order, and the order is the
// contract in addon.h:
//
//   1. off the display -- LVGL never draws a widget while the code that owns it
//      is being unmapped;
//   2. every record of the screen cleared (back-stack, guards, the sheet's
//      memory of the page underneath) -- these are raw pointers into what comes
//      next;
//   3. the addon's stop(): its threads joined, its timers deleted, its player
//      hooks withdrawn, its files closed;
//   4. the screens deleted -- after stop(), because stop() is allowed to look at
//      its own widgets;
//   5. dlclose(): the mapping of a file on the card goes away, which is the
//      point of the exercise -- a mapped .so keeps the filesystem alive after
//      the card leaves, and every read of that dead mount blocks.
// ---------------------------------------------------------------------------

// Where the library that owns `fn` sits in memory, from the loader's own
// segment list. Needed for one thing only: dropping the calls its workers posted
// into the GUI queue a moment before they were stopped. Those callbacks are code
// in the library, and dlclose() takes the code away.
typedef struct {
	void *base;
	uintptr_t lo, hi;
	bool found;
} lib_range_t;

static int lib_range_cb(struct dl_phdr_info *info, size_t size, void *user) {
	(void)size;
	lib_range_t *range = user;
	if ((void *)info->dlpi_addr != range->base) {
		return 0;
	}
	for (int i = 0; i < info->dlpi_phnum; i++) {
		if (info->dlpi_phdr[i].p_type != PT_LOAD) {
			continue;
		}
		uintptr_t start = (uintptr_t)info->dlpi_addr + (uintptr_t)info->dlpi_phdr[i].p_vaddr;
		uintptr_t end = start + (uintptr_t)info->dlpi_phdr[i].p_memsz;
		if (!range->found || start < range->lo) {
			range->lo = start;
		}
		if (!range->found || end > range->hi) {
			range->hi = end;
		}
		range->found = true;
	}
	return 1; // the first mapping with that base is the one we want
}

static void drop_pending_calls(const sonix_addon_stop_t *stop) {
	Dl_info info;
	if (!stop || !dladdr((void *)(uintptr_t)stop->stop, &info) || !info.dli_fbase) {
		return;
	}
	lib_range_t range = {.base = info.dli_fbase};
	dl_iterate_phdr(lib_range_cb, &range);
	if (range.found) {
		gui_post_drop_range((const void *)range.lo, (const void *)range.hi);
	}
}

static void registry_clear(void) {	for (int i = 0; i < slot_count; i++) {
		if (slots[i].art.data) {
			free((void *)slots[i].art.data);
		}
	}
	slot_count = 0;
	memset(slots, 0, sizeof(slots));
	memset(info, 0, sizeof(info));
}

bool addon_unload(int index) {
	if (index < 0 || index >= slot_count) {
		return false;
	}
	addon_slot_t *slot = &slots[index];
	if (!slot->loaded) {
		return true; // never opened: nothing is mapped, nothing runs
	}

	// 1 and 2. Away from it first, and out of everything that remembers it. The
	// first screen is init()'s and is always known; the rest are whatever the
	// addon registered as it opened them.
	for (int i = 0; i < slot->screen_count; i++) {
		lv_obj_t *s = slot->screens[i];
		if (!s) {
			continue;
		}
		if (gui_obj_alive(s) && lv_screen_active() == s) {
			switch_screen_no_history(main_menu_screen);
		}
		// Records are pointers, not objects: cleared whether or not the screen
		// still exists.
		switcher_forget_screen(s);
		player_forget_screen(s);
	}

	// 3. Its own teardown. Mandatory since the card made unloading a thing:
	// lib_open() refuses a library with no stop hook precisely so that this
	// cannot be skipped by accident.
	bool safe = true;
	if (slot->stop) {
		safe = slot->stop();
		if (!safe) {
			// A worker is still inside the library -- a socket the player cannot
			// interrupt is the usual reason. Unmapping now would make its next
			// instruction a jump into nothing, so the mapping stays. What is lost
			// is the card's unmount (a mapped .so pins the filesystem) and this
			// addon for the rest of the run; what is not lost is the interface.
			fprintf(stderr, "addon: %s says it is not safe to unload; keeping the library mapped\n", slot->id);
		}
	} else {
		fprintf(stderr, "addon: %s has no stop hook; its threads may still be running\n", slot->id);
	}

	// Whatever the service table still points at in this library. Unregistered
	// here rather than trusted to the addon's stop(): a stale table entry is a
	// dereference of unmapped memory on the next star, and the cost of asking
	// twice is nothing (the call by id is a no-op when absent).
	service_unregister(slot->id);

	// 4. The screens, registered ones included. Deleted whether or not the addon
	// already did (gui_obj_alive() answers that); LVGL drops the widget tree with
	// them, and their event callbacks are in the library.
	for (int i = 0; i < slot->screen_count; i++) {
		lv_obj_t *s = slot->screens[i];
		if (s && gui_obj_alive(s)) {
			lv_obj_delete(s);
		}
		slot->screens[i] = NULL;
	}
	slot->screen_count = 0;
	slot->screen = NULL;

	// 4b. Anything the library's workers posted before they were stopped: those
	// callbacks are code in the library and the next step unmaps it. Done after
	// stop() is the only correct place -- a join may post.
	drop_pending_calls(slot->stop_lib);

	if (!safe) {
		// Deliberately no dlclose(): the handle is forgotten with the rest below,
		// which leaks the mapping on purpose. The tile goes too, so nothing can
		// call into it again.
		slot->unload_refused = true;
		slot->loaded = false;
		slot->handle = NULL;
		slot->lib = NULL;
		slot->stop = NULL;
		slot->stop_lib = NULL;
		memset(&slot->lib_copy, 0, sizeof(slot->lib_copy));
		return false;
	}

	// 5. The library itself.
	bool closed = true;
	if (slot->handle) {
		if (dlclose(slot->handle) != 0) {
			const char *why = dlerror();
			fprintf(stderr, "addon: %s stays loaded: %s\n", slot->id, why ? why : "dlclose failed");
			closed = false;
		}
	}

	slot->loaded = false;
	slot->handle = NULL;
	slot->lib = NULL;
	slot->stop = NULL;
	slot->stop_lib = NULL;
	memset(&slot->lib_copy, 0, sizeof(slot->lib_copy));

	fprintf(stderr, "addon: %s stopped%s\n", slot->id, closed ? "" : " (library still mapped)");
	return closed;
}

// Is the file this slot was opened from still the one on the card? False when
// the card is gone, which is the same answer: nothing there is ours any more.
static bool slot_files_current(const addon_slot_t *slot) {
	char path[ADDON_PATH_MAX];
	snprintf(path, sizeof(path), "%s/addon.so", slot->dir);
	file_id_t now;
	if (!file_id_read(path, &now) || !file_id_equal(&now, &slot->so_id)) {
		return false;
	}
	snprintf(path, sizeof(path), "%s/addon.json", slot->dir);
	if (!file_id_read(path, &now) || !file_id_equal(&now, &slot->manifest_id)) {
		return false;
	}
	return true;
}

// ---------------------------------------------------------------------------
// The scan
// ---------------------------------------------------------------------------

static void slot_read(const char *sd_root, const char *entry_name) {
	addon_slot_t *slot = &slots[slot_count];

	if (strlen(entry_name) >= sizeof(slot->id)) {
		fprintf(stderr, "addon: '%s' is a longer name than an addon can have\n", entry_name);
		return;
	}
	snprintf(slot->id, sizeof(slot->id), "%s", entry_name);
	snprintf(slot->dir, sizeof(slot->dir), "%s/Addons/%s", sd_root, entry_name);

	char path[ADDON_PATH_MAX];
	snprintf(path, sizeof(path), "%s/addon.json", slot->dir);
	if (!manifest_read(path, slot)) {
		return;
	}
	file_id_read(path, &slot->manifest_id);

	// A manifest with no library behind it is a half-copied folder. The tile
	// would open a sentence about a missing file; better to have no tile.
	snprintf(path, sizeof(path), "%s/addon.so", slot->dir);
	if (!file_exists(path)) {
		fprintf(stderr, "addon: %s has no addon.so\n", slot->id);
		return;
	}
	// Remembered now, so that a tap later can tell whether the file it is about
	// to dlopen() is still this one.
	file_id_read(path, &slot->so_id);

	// A service's paths are the player's business before this library is ever
	// opened: USB export, the sleep timer and the factory reset ask during boot.
	if (slot->is_service || slot->owns_count > 0) {
		service_register_paths(slot->id, slot->owns_paths, slot->owns_count,
							   slot->cache_path[0] ? slot->cache_path : NULL);
	}

	// The manifest may name its picture; icon.png is what it is called when it
	// does not say.
	char icon_name[128] = "icon.png";
	char manifest[ADDON_MANIFEST_MAX];
	snprintf(path, sizeof(path), "%s/addon.json", slot->dir);
	if (read_small_file(path, manifest, sizeof(manifest), NULL)) {
		json_doc_t doc;
		if (json_parse(manifest, &doc)) {
			char named[128] = "";
			if (json_obj_str(&doc, json_root(&doc), "icon", named, sizeof(named)) && named[0]) {
				snprintf(icon_name, sizeof(icon_name), "%s", named);
			}
			json_free(&doc);
		}
	}
	icon_load(slot, icon_name);

	slot->present = true;
	fprintf(stderr, "addon: %s%s '%s' (%s)\n", slot->id, slot->version[0] ? " " : "", slot->title,
			slot->section == ADDON_SECTION_STREAMING ? "streaming" : "more");
	slot_count++;
}

// A second scan does not carry libraries over: a scan happens because the card
// changed (insert, or a card that came back), and a library mapped from the
// previous mount is exactly what must not survive it. The tiles keep their
// place; the code is opened again on the next tap.
void addon_scan(const gui_config_t *cfg) {
	if (!cfg || !cfg->sd_root_path || !cfg->sd_root_path[0]) {
		return; // no card, no place to look: what the registry holds stays
	}

	char dir[ADDON_PATH_MAX];
	snprintf(dir, sizeof(dir), "%s/Addons", cfg->sd_root_path);

	// Read the folder BEFORE the registry is touched. A folder that cannot be
	// read -- card out, not mounted yet, no Addons/ at all -- is not a scan that
	// found nothing, it is a scan that could not look, and replacing a good
	// registry with an empty one is how the tiles disappeared for good after a
	// card was pulled and put back.
	DIR *d = opendir(dir);
	if (!d) {
		fprintf(stderr, "addon: cannot read %s: %s\n", dir, strerror(errno));
		return;
	}

	int previous = slot_count;

	// The pictures of the previous scan are ours alone: the icon is an
	// lv_image_dsc_t whose data came from calloc() in icon_pack(), and the icon
	// pointers the pages hold are about to be dropped anyway.
	for (int i = 0; i < previous; i++) {
		if (slots[i].art.data) {
			free((void *)slots[i].art.data);
		}
	}

	slot_count = 0;
	memset(slots, 0, sizeof(slots));
	memset(info, 0, sizeof(info));

	// Kept for addon_card_changed(): the panel does not change while the player
	// runs, and the mount point may (a card under another firmware name).
	// memcpy and not assignment: the fields of gui_config_t are const, so the
	// structure cannot be assigned to, only copied into.
	if (cfg != &scanned_cfg) {
		memcpy(&scanned_cfg, cfg, sizeof(scanned_cfg));
	}
	// The root is copied into our own buffer and the structure pointed at it:
	// the caller's cfg may be a copy on a stack that is gone by the time an
	// addon reads host->cfg or host->sd_root. Never through snprintf() when the
	// two are already the same buffer -- that is a copy onto itself, it is
	// undefined, and glibc answers it with an empty string, which then turns
	// every addon path into "/Addons/...".
	if (cfg->sd_root_path != scanned_root) {
		snprintf(scanned_root, sizeof(scanned_root), "%s", cfg->sd_root_path);
	}
	scanned_cfg.sd_root_path = scanned_root;

	host.id = NULL;
	host.dir = NULL;
	host.sd_root = scanned_root;
	host.cfg = &scanned_cfg;

	struct dirent *e;
	while ((e = readdir(d)) != NULL && slot_count < ADDON_MAX) {
		if (e->d_name[0] == '.') {
			continue;
		}
		char sub[ADDON_PATH_MAX];
		snprintf(sub, sizeof(sub), "%s/%s", dir, e->d_name);
		struct stat st;
		if (stat(sub, &st) != 0 || !S_ISDIR(st.st_mode)) {
			continue;
		}
		slot_read(scanned_root, e->d_name);
	}
	closedir(d);

	for (int i = 0; i < slot_count; i++) {
		info[i].id = slots[i].id;
		info[i].title = slots[i].title;
		info[i].section = slots[i].section;
		info[i].icon = slots[i].icon;
		info[i].order = slots[i].order;
		info[i].is_service = slots[i].is_service;
		info[i].owns_count = slots[i].owns_count;
		info[i].owns_paths = slots[i].owns_paths;
		info[i].cache_path = slots[i].cache_path[0] ? slots[i].cache_path : NULL;
	}
}

// The card came or went. Called from the storage layer, on its own thread: the
// folder is read and the pages are rebuilt on the UI thread, because both touch
// LVGL.
static void card_changed_on_ui(void *user) {
	(void)user;
	// Nothing mapped from the previous mount survives an insert. Even when the
	// files look identical the mapping points at the mount that has gone, and a
	// second dlopen() of the same path would put two copies of one addon in the
	// process -- two sets of statics, two workers, one set of files. The tiles
	// are rebuilt below and the code is opened again on the next tap.
	for (int i = 0; i < slot_count; i++) {
		addon_unload(i);
	}
	// scanned_cfg already holds the root, in scanned_root: handing addon_scan()
	// a stack copy of it would only re-introduce the pointer that has to outlive
	// the call.
	addon_scan(&scanned_cfg);
	if (refresh_hook) {
		refresh_hook();
	}
}

void addon_card_changed(const char *root) {
	if (root && root[0]) {
		snprintf(scanned_root, sizeof(scanned_root), "%s", root);
	}
	gui_post(card_changed_on_ui, NULL);
}

// Synchronous on purpose. The storage layer calls this on its way to unmounting
// the card, and the unmount has to happen after the last mapping of a file on
// that card is gone: otherwise umount(2) fails with EBUSY, the fallback is
// MNT_DETACH, and the dead filesystem stays alive inside the process, blocking
// every read that still lands on it.
//
// The wait is bounded: a GUI thread stuck in an operation of its own must not
// take the storage thread -- and with it the whole card path -- down with it.
#define ADDON_UNLOAD_WAIT_MS 4000

static void card_gone_on_ui_checked(void *user) {
	bool *ok = user;
	*ok = true;
	for (int i = 0; i < slot_count; i++) {
		if (!addon_unload(i)) {
			*ok = false; // a library stayed mapped: the unmount will fall back
		}
	}
	registry_clear();
	if (refresh_hook) {
		refresh_hook();
	}
}

bool addon_card_gone(void) {
	bool ok = false;
	if (!gui_call(card_gone_on_ui_checked, &ok, ADDON_UNLOAD_WAIT_MS)) {
		fprintf(stderr, "addon: the card is leaving before the addons could be unloaded\n");
		return false;
	}
	return ok;
}

int addon_count(void) { return slot_count; }

const addon_info_t *addon_get(int index) {
	if (index < 0 || index >= slot_count) {
		return NULL;
	}
	return &info[index];
}

void sonix_addon_own_screen(const char *id, lv_obj_t *screen) {
	if (!id || !id[0] || !screen) {
		return;
	}
	for (int i = 0; i < slot_count; i++) {
		if (strcmp(slots[i].id, id) != 0) {
			continue;
		}
		for (int s = 0; s < slots[i].screen_count; s++) {
			if (slots[i].screens[s] == screen) {
				return; // registered twice: the list is a set
			}
		}
		if (slots[i].screen_count >= ADDON_SCREENS_MAX) {
			fprintf(stderr, "addon: %s owns more than %d screens; '%p' will not be unloaded\n", id,
					ADDON_SCREENS_MAX, (void *)screen);
			return;
		}
		slots[i].screens[slots[i].screen_count++] = screen;
		return;
	}
	fprintf(stderr, "addon: '%s' registered a screen but is not in the registry\n", id);
}

// ---------------------------------------------------------------------------
// Opening one
// ---------------------------------------------------------------------------

static void open_failed(const addon_slot_t *slot, const char *why) {
	// One key for all of it: what an addon writer gets wrong is a wrong API
	// number, a missing symbol, an init() that gives up, and the sentence
	// names the addon and the reason either way.
	char message[320];
	lv_snprintf(message, sizeof(message), tr("l'addon %s non è partito: %s"), slot->title,
				why && why[0] ? why : "no reason");
	gui_notify_popup(message);
}

static bool lib_open(addon_slot_t *slot) {
	char path[ADDON_PATH_MAX];
	snprintf(path, sizeof(path), "%s/addon.so", slot->dir);

	void *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
	if (!handle) {
		const char *why = dlerror();
		fprintf(stderr, "addon: dlopen %s: %s\n", path, why ? why : "no reason given");
		// dlerror() prefixes its sentence with the path of the library -- once,
		// and sometimes twice -- and the user is looking at the addon's name
		// already: what is useful starts after the last "addon.so: ".
		const char *detail = why;
		if (why) {
			const char *at = strstr(why, "addon.so: ");
			if (at) {
				detail = at + strlen("addon.so: ");
			}
		}
		open_failed(slot, detail);
		return false;
	}

	dlerror();
	sonix_addon_entry_fn entry = (sonix_addon_entry_fn)(void (*)(void))dlsym(handle, "sonix_addon_entry");
	const char *why = dlerror();
	if (!entry || why) {
		fprintf(stderr, "addon: %s has no sonix_addon_entry\n", path);
		dlclose(handle);
		open_failed(slot, "sonix_addon_entry");
		return false;
	}

	const sonix_addon_t *lib = entry(SONIX_ADDON_API);
	if (!lib) {
		fprintf(stderr, "addon: %s refused API %d\n", path, SONIX_ADDON_API);
		dlclose(handle);
		open_failed(slot, "API mismatch");
		return false;
	}
	if (lib->api != SONIX_ADDON_API || !lib->init) {
		fprintf(stderr, "addon: %s returned a descriptor for API %u with no init\n", path, lib->api);
		dlclose(handle);
		open_failed(slot, "API mismatch");
		return false;
	}

	// The way out, and the reason a library is allowed in at all: without a stop
	// hook it could never be unloaded, so it would pin the card and outlive every
	// removal. Refused at the door rather than discovered at the worst moment.
	dlerror();
	sonix_addon_stop_entry_fn stop_entry = (sonix_addon_stop_entry_fn)(void (*)(void))dlsym(handle, SONIX_ADDON_STOP_ENTRY);
	if (!stop_entry || dlerror()) {
		fprintf(stderr, "addon: %s has no %s\n", path, SONIX_ADDON_STOP_ENTRY);
		dlclose(handle);
		open_failed(slot, "not hot-pluggable: no stop hook");
		return false;
	}
	const sonix_addon_stop_t *stop = stop_entry(SONIX_ADDON_API);
	if (!stop || stop->api != SONIX_ADDON_API || !stop->stop) {
		fprintf(stderr, "addon: %s offered no usable stop hook\n", path);
		dlclose(handle);
		open_failed(slot, "stop hook: API mismatch");
		return false;
	}

	// The service half, if there is one. A second symbol rather than a field in
	// sonix_addon_t, so that an app addon built before services existed keeps
	// loading: its absence is the answer "an app".
	dlerror();
	sonix_addon_service_entry_fn service_entry =
		(sonix_addon_service_entry_fn)(void (*)(void))dlsym(handle, SONIX_ADDON_SERVICE_ENTRY);
	if (!dlerror() && service_entry) {
		const sonix_addon_service_t *service = service_entry(SONIX_ADDON_API);
		if (service && service->id) {
			service_register(service);
		} else if (slot->is_service) {
			fprintf(stderr, "addon: %s says it is a service but exports no table\n", slot->id);
		}
	}

	// Copied: the descriptor lives in the library, and the shell reads it on
	// every tile tap; a copy keeps that read in our own memory.
	slot->lib_copy = *lib;
	slot->lib = &slot->lib_copy;
	slot->stop_lib = stop;
	slot->stop = stop->stop;
	slot->handle = handle;

	host.id = slot->id;
	host.dir = slot->dir;

	lv_obj_t *screen = slot->lib_copy.init(&host);
	if (!screen) {
		fprintf(stderr, "addon: %s init() gave no screen\n", slot->id);
		slot->lib = NULL;
		slot->handle = NULL;
		slot->stop = NULL;
		slot->stop_lib = NULL;
		dlclose(handle);
		open_failed(slot, "init failed");
		return false;
	}
	slot->screen = screen;
	// The shell knows this one by return value; the rest, if any, arrive through
	// sonix_addon_own_screen() as the addon opens them. Appended to whatever the
	// addon registered while init() ran -- overwriting the list here would throw
	// away the sub-screens a well-behaved addon had just registered.
	sonix_addon_own_screen(slot->id, screen);

	// What is open is what was on the card at this moment: re-read the identity
	// after the dlopen(), so that the next tap compares against the file that was
	// actually mapped rather than one the scan saw a copy ago.
	file_id_read(path, &slot->so_id);
	snprintf(path, sizeof(path), "%s/addon.json", slot->dir);
	file_id_read(path, &slot->manifest_id);

	slot->loaded = true;
	fprintf(stderr, "addon: %s opened\n", slot->id);
	return true;
}

bool addon_open(int index) {
	if (index < 0 || index >= slot_count) {
		return false;
	}
	addon_slot_t *slot = &slots[index];

	if (slot->unload_refused) {
		// Its stop() said a thread was still inside the library, so the mapping
		// was kept and this addon spends the rest of the run unavailable. Opening
		// it again would build a second copy of everything on top of the first.
		fprintf(stderr, "addon: %s was left loaded and is not opened again\n", slot->id);
		return false;
	}

	// Hot-plug: the tile is the point where the question "is this still the
	// library on the card?" gets answered. An addon copied over adb, downloaded
	// over the network or carried in on a card runs the new code here, with no
	// restart of the player -- which is what an addon folder is for.
	if (slot->loaded && !slot_files_current(slot)) {
		fprintf(stderr, "addon: %s changed on the card, reloading\n", slot->id);
		if (!addon_unload(index)) {
			// stop() said a thread is still inside the library. Unloading was
			// refused and the mapping kept: opening a fresh copy on top of it
			// would be two of everything, which is the case unload_refused
			// exists to prevent -- so check it here, not only on entry.
			fprintf(stderr, "addon: %s could not be reloaded; the old library stays\n", slot->id);
			return false;
		}
	}

	if (!slot->loaded && !lib_open(slot)) {
		return false;
	}
	if (!slot->screen || !gui_obj_alive(slot->screen)) {
		// The addon's screen was deleted while it was open -- by a bug in the
		// addon, since only the addon knows about it. Say so instead of
		// switching to a dangling pointer.
		fprintf(stderr, "addon: %s screen is gone\n", slot->id);
		open_failed(slot, "screen missing");
		return false;
	}
	switch_screen(slot->screen);
	return true;
}

// ---------------------------------------------------------------------------
// The tiles
// ---------------------------------------------------------------------------

static void addon_tile_cb(void *user) {
	addon_open((int)(intptr_t)user);
}

int addon_fill_entries(addon_section_t section, void *entries, int max) {
	grid_entry_t *out = entries;
	int n = 0;
	for (int i = 0; i < slot_count && n < max; i++) {
		if (slots[i].section != section) {
			continue;
		}
		tile_actions[i].fn = addon_tile_cb;
		tile_actions[i].arg = (void *)(intptr_t)i;
		grid_entry_t entry = {
			.label = slots[i].title,
			.icon = slots[i].icon,
			.target = NULL,
			.action = NULL,
			.action_user = &tile_actions[i],
			.order = slots[i].order,
		};
		out[n++] = entry;
	}
	return n;
}
