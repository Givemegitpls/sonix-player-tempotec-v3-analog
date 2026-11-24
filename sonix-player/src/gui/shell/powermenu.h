#ifndef POWERMENU_H
#define POWERMENU_H

#include <stdbool.h>

#include "src/gui/shell/gui.h"

// What a long press on the power key brings up: shut down, restart, or back
// out. A short press still just toggles the screen.
void powermenu_init(gui_config_t *cfg);

// The restart the menu offers, as a function: playback down, caches cleared, the
// clock into the RTC, the card released, then init reboots the device. Used by
// the player update, which has prepared the rootfs and needs the same shutdown
// the user's own "restart" gets -- a raw reboot() from inside the process skips
// init's hooks.
void powermenu_reboot_now(void);

void powermenu_show(void);
bool powermenu_is_open(void);

// Safe to call from the input thread: the panel is raised on the UI thread via
// lv_async_call rather than from underneath it.
void powermenu_request_show(void);

#endif /* POWERMENU_H */
