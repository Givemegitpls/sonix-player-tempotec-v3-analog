#ifndef WIFINET_H
#define WIFINET_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The two things on the Wi-Fi page that need a page of their own: joining a
// network that does not broadcast its name, and the addresses the resolver
// asks.
//
// Both are rows in the page the caller already has a container for, rather than
// a page of their own, so they sit where the rest of the settings sit and are
// drawn by the same widgets. The one shared text-entry page is built on first
// use: the SSID of the manual network, its passphrase, and each of the two DNS
// addresses are four questions of the same shape.

// The "hidden network" row: asks for a name, then a passphrase, then joins it.
// The caller places it with the networks, above the list of what is in the air.
void wifinet_add_manual_row(lv_obj_t *container, gui_config_t *cfg);

// One row on the Wi-Fi page that opens the two addresses. Placed under the switch,
// it shows the address in use and is re-read whenever a page is shown, so a lease
// that changed it is not hidden behind a stale row.
void wifinet_add_dns_row(lv_obj_t *container, gui_config_t *cfg);

#endif /* WIFINET_H */
