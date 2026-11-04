#include "streaming.h"

#include "lvgl/lvgl.h"

#include "src/addon/addon.h"
#include "src/gui/shell/gridpage.h"
#include "src/gui/shell/icons_select.h"
#include "src/gui/streaming/podcastpage.h"
#include "src/gui/streaming/radiopage.h"
#include "src/gui/shell/settingsrow.h"

lv_obj_t *streaming_screen;

// The grid and the config, kept for streaming_refresh(): the addons on the card
// can arrive after this page was built, and the tile set is what changes then.
static lv_obj_t *streaming_grid;
static gui_config_t *streaming_cfg;

// The four that are compiled in, and then whatever the card brought: each addon
// asks for its section in its manifest. Past two rows the page scrolls rather
// than growing a seventh tile off the bottom edge.
static int build_entries(grid_entry_t *entries, int max) {
	(void)max;
	int count = 0;
	entries[count++] = (grid_entry_t){.label = "radio", .icon = &icon_menu_radio, .target = &radiopage_screen};
	entries[count++] = (grid_entry_t){.label = "podcasts", .icon = &icon_menu_podcast, .target = &podcast_screen};
	count += addon_fill_entries(ADDON_SECTION_STREAMING, entries + count, ADDON_MAX);
	return count;
}

void streaming_refresh(void) {
	if (!streaming_grid || !streaming_cfg) {
		return; // nothing can have changed before the page was built
	}
	grid_entry_t entries[4 + ADDON_MAX];
	int count = build_entries(entries, 4 + ADDON_MAX);
	gridpage_set_entries(streaming_grid, streaming_cfg, entries, count, 2, 3, true);
	if (count > 6) {
		gridpage_set_scrolling(streaming_grid);
	} else {
		lv_obj_set_scrollable(streaming_grid, false);
		lv_obj_set_scrollbar_mode(streaming_grid, LV_SCROLLBAR_MODE_OFF);
	}
}

void streaming_init(gui_config_t *cfg) {
	podcastpage_init(cfg);
	streaming_cfg = cfg;

	grid_entry_t entries[4 + ADDON_MAX];
	int count = build_entries(entries, 4 + ADDON_MAX);

	// Two columns and three rows of tile, exactly like Music and Wireless, so a
	// tile here is the same size as a tile there. Four of them fill the first
	// two rows; the third stays empty rather than the tiles growing to fill it.
	streaming_grid = gridpage_build(streaming_screen, cfg, entries, count, 2, 3, true);
	if (count > 6) {
		gridpage_set_scrolling(streaming_grid);
	}

	settingsrow_title(streaming_screen, cfg, "streaming");
}
