#include "morepage.h"

#include "lvgl/lvgl.h"

#include "src/addon/addon.h"
#include "src/gui/audio/dacpage.h"
#include "src/gui/audio/metronomepage.h"
#include "src/gui/ebook/ebookpage.h"
#include "src/gui/library/filespage.h"
#include "src/gui/shell/gridpage.h"
#include "src/gui/shell/icons_select.h"
#include "src/gui/shell/settingsrow.h"

lv_obj_t *morepage_screen;

// The grid and the config, kept for morepage_refresh(): the addons on the card
// can arrive after this page was built.
static lv_obj_t *morepage_grid;
static gui_config_t *morepage_cfg;

// The five that are compiled in, plus the card's: an addon whose manifest says
// "more" is an app like any of these, and it is appended after them so that the
// built-in order never depends on what is in the slot. Past six entries the page
// scrolls.
//
// Gearboy and Flappy Bird used to be two of these. They are addons now
// (addons/gearboy, addons/flappybird), and their manifests ask for the places
// this page used to give them (`order`), so the page looks the same.
static int build_entries(grid_entry_t *entries, int max) {
	(void)max;
	int count = 0;
	entries[count++] = (grid_entry_t){.label = "dac", .icon = &icon_menu_dac, .target = &dacpage_screen};
	// An action rather than a screen: the shelf reads the Ebook folder again
	// every time it is opened, so a book copied onto the card turns up
	// without a restart.
	entries[count++] = (grid_entry_t){.label = "books", .icon = &icon_menu_books, .action = ebookpage_open};
	// An action for the same reason the shelf is one: the listing is read
	// when the page is entered, so a card that changed under the player
	// shows what is on it now.
	entries[count++] =
		(grid_entry_t){.label = "file_explorer", .icon = &icon_menu_file_explorer, .action = filespage_open};
	entries[count++] =
		(grid_entry_t){.label = "metronome", .icon = &icon_menu_metronome, .target = &metronomepage_screen};
	count += addon_fill_entries(ADDON_SECTION_MORE, entries + count, ADDON_MAX);
	return count;
}

void morepage_refresh(void) {
	if (!morepage_grid || !morepage_cfg) {
		return; // nothing can have changed before the page was built
	}
	grid_entry_t entries[4 + ADDON_MAX];
	int count = build_entries(entries, 4 + ADDON_MAX);
	gridpage_set_entries(morepage_grid, morepage_cfg, entries, count, 2, 3, true);
	if (count > 6) {
		// Four built-in entries fill two thirds of this page; the first addon
		// puts a fifth under them, so the page has to scroll to be reachable
		// at all.
		gridpage_set_scrolling(morepage_grid);
	} else {
		lv_obj_set_scrollable(morepage_grid, false);
		lv_obj_set_scrollbar_mode(morepage_grid, LV_SCROLLBAR_MODE_OFF);
	}
}

void morepage_init(gui_config_t *cfg) {
	morepage_cfg = cfg;

	grid_entry_t entries[4 + ADDON_MAX];
	int count = build_entries(entries, 4 + ADDON_MAX);

	// Two columns by three rows, like the Music and Wireless pages, so a tile
	// here is exactly the size of a tile there. The grid is laid out as full
	// even with fewer entries, so the only tile sits where it would with six --
	// top left -- instead of floating in the middle of a page of its own.
	morepage_grid = gridpage_build(morepage_screen, cfg, entries, count, 2, 3, true);
	if (count > 6) {
		gridpage_set_scrolling(morepage_grid);
	}

	settingsrow_title(morepage_screen, cfg, "more");
}
