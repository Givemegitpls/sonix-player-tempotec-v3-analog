#include "wireless.h"

#include "lvgl/lvgl.h"

#include "src/addon/addon.h"
#include "src/gui/wireless/airplay.h"
#include "src/gui/bluetooth/btsettings.h"
#include "src/gui/wireless/dlna.h"
#include "src/gui/shell/gridpage.h"
#include "src/gui/wireless/sonixlink.h"
#include "src/gui/shell/icons_select.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/wireless/wifisettings.h"
#include "src/gui/wireless/wifitransfer.h"

lv_obj_t *wireless_screen;

// The grid and the config, kept for wireless_refresh(): the addons on the card
// can arrive after this page was built, and the tile set is what changes then.
static lv_obj_t *wireless_grid;
static gui_config_t *wireless_cfg;

// The six that are compiled in, and then whatever the card brought: an addon
// whose manifest says "wireless" is a page about the radios like these are, and
// it is appended after them so that the built-in order never depends on what is
// in the slot. Past six entries the page scrolls.
static int build_entries(grid_entry_t *entries, int max) {
	int count = 0;
	entries[count++] = (grid_entry_t){.label = "wi_fi", .icon = &icon_menu_wifi_settings, .target = &wifisettings_screen};
	entries[count++] = (grid_entry_t){.label = "bluetooth", .icon = &icon_menu_bluetooth, .target = &btsettings_screen};
	entries[count++] = (grid_entry_t){.label = "airplay", .icon = &icon_menu_airplay, .target = &airplay_screen};
	entries[count++] = (grid_entry_t){.label = "transfer", .icon = &icon_menu_wifi_transfer, .target = &wifitransfer_screen};
	entries[count++] = (grid_entry_t){.label = "sonixlink", .icon = &icon_menu_sonixlink, .target = &sonixlink_screen};
	entries[count++] = (grid_entry_t){.label = "dlna", .icon = &icon_menu_dlna, .target = &dlna_screen};
	count += addon_fill_entries(ADDON_SECTION_WIRELESS, entries + count, max - count);
	return count;
}

void wireless_refresh(void) {
	if (!wireless_grid || !wireless_cfg) {
		return; // nothing can have changed before the page was built
	}
	grid_entry_t entries[6 + ADDON_MAX];
	int count = build_entries(entries, 6 + ADDON_MAX);
	gridpage_set_entries(wireless_grid, wireless_cfg, entries, count, 2, 3, true);
	if (count > 6) {
		gridpage_set_scrolling(wireless_grid);
	} else {
		lv_obj_set_scrollable(wireless_grid, false);
		lv_obj_set_scrollbar_mode(wireless_grid, LV_SCROLLBAR_MODE_OFF);
	}
}

void wireless_init(gui_config_t *cfg) {
	wireless_cfg = cfg;

	grid_entry_t entries[6 + ADDON_MAX];
	int count = build_entries(entries, 6 + ADDON_MAX);

	// Two columns and three rows' worth of tile, like the Music section, so a
	// tile here is exactly the size of a tile there. Six entries fill the grid
	// exactly; an addon on this page starts a second one below, and the page
	// scrolls to it.
	wireless_grid = gridpage_build(wireless_screen, cfg, entries, count, 2, 3, true);
	if (count > 6) {
		gridpage_set_scrolling(wireless_grid);
	}

	settingsrow_title(wireless_screen, cfg, "wireless");
}
