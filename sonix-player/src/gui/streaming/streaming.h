#ifndef STREAMING_H
#define STREAMING_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The Streaming section: the same tiled page as Music and Wireless, with a
// tile each for Tidal, Qobuz, internet radio and podcasts.
extern lv_obj_t *streaming_screen;

void streaming_init(gui_config_t *cfg);

// Puts the tiles up again, addons included. For the card arriving after the
// player started: the grid is the page's, so the page is what rebuilds it.
void streaming_refresh(void);

#endif /* STREAMING_H */
