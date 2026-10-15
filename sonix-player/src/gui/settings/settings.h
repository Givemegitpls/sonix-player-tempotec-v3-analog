#ifndef SETTINGS_H
#define SETTINGS_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

extern lv_obj_t *settings_screen;

void settings_init(gui_config_t *cfg);

// Hands power.c the screen timeout as it was last left ([power]
// screen_off_seconds). Call once at startup, after power_init(); the Screen
// page calls it again whenever the slider moves.
void settings_apply_screen_off(void);

// The screen-off timeout the Screen page starts from, and the one power_init
// applies before that page has been built: [power] screen_off_seconds. Both
// readers share this one value, so the startup line in the log cannot report a
// timeout the player is not using.
#define SCREEN_OFF_DEFAULT_S 30

// Shows or hides the developer options entry according to whether it has been
// unlocked. The page is built once at startup, so it has to be told when the
// five taps on the build number unlock it.
void settings_refresh_devoptions(void);

#endif // SETTINGS_H
