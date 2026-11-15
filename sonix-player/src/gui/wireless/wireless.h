#ifndef WIRELESS_H
#define WIRELESS_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The Wireless section: the same tiled page the main menu and Music use. Wi-Fi,
// Bluetooth, AirPlay, Wi-Fi transfer, DLNA and SonixLink each open their own
// page; addons whose manifest says "wireless" follow them.

extern lv_obj_t *wireless_screen;

void wireless_init(gui_config_t *cfg);

// Puts the tiles up again, addons included. For the card arriving after the
// player started: the grid is the page's, so the page is what rebuilds it.
void wireless_refresh(void);

#endif /* WIRELESS_H */
