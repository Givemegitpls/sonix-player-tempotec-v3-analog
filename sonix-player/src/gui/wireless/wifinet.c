#include "wifinet.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lvgl/lvgl.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/keyboard.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/gui/wireless/wifisettings.h"
#include "src/system/core/lang.h"
#include "src/system/net/netdns.h"
#include "src/system/net/wifi.h"

// The keyboard the player's other text pages use, so a field typed on one looks
// like a field typed on the next.
#define WIFINET_KEYBOARD_H SC(316)

// wpa_supplicant's own limit on a network name. The buffer is larger (it also
// carries what a scan reported), so the check is against this and not against
// WIFI_SSID_MAX.
#define WIFINET_SSID_MAX 32

static gui_config_t *page_cfg;

// ---------------------------------------------------------------------------
// the one entry page
// ---------------------------------------------------------------------------

static lv_obj_t *entry_screen;
static lv_obj_t *entry_title;
static lv_obj_t *entry_field;
static keyboard_t *entry_keyboard;

// What to do with what was typed. Runs on the UI thread, from the OK key, and
// may return without closing the page -- a name that cannot be an SSID or an
// address that is not an address leaves the field up to be corrected.
static void (*entry_done)(const char *text, void *user);
static void *entry_user;

static void entry_accept_cb(lv_event_t *e) {
	(void)e;
	if (entry_done) {
		entry_done(lv_textarea_get_text(entry_field), entry_user);
	}
}

static void entry_build(void) {
	entry_screen = lv_obj_create(NULL);
	lv_obj_add_style(entry_screen, &theme_style_screen, 0);

	// The heading is the question -- the network's name, or which of the two
	// addresses is being asked for -- so it is a label the opener rewrites.
	entry_title = settingsrow_title(entry_screen, page_cfg, "");

	int top = settingsrow_content_top(page_cfg);
	entry_field = lv_textarea_create(entry_screen);
	lv_textarea_set_one_line(entry_field, true);
	lv_obj_set_size(entry_field, page_cfg->screen_width - 2 * page_cfg->padding, SC(62));
	lv_obj_set_scrollbar_mode(entry_field, LV_SCROLLBAR_MODE_OFF);
	lv_obj_align(entry_field, LV_ALIGN_TOP_LEFT, page_cfg->padding, top + SC(10));
	lv_obj_add_style(entry_field, &theme_style_card, 0);
	lv_obj_set_style_radius(entry_field, SC(12), 0);
	lv_obj_set_style_border_width(entry_field, 0, 0);
	lv_obj_set_style_shadow_width(entry_field, 0, 0);
	lv_obj_set_style_pad_all(entry_field, SC(14), 0);
	lv_obj_set_style_text_font(entry_field, &font_ui_24, 0);
	keyboard_style_caret(entry_field);

	entry_keyboard = keyboard_create(entry_screen, page_cfg->screen_width, WIFINET_KEYBOARD_H, entry_field, NULL, "ok",
									 entry_accept_cb, NULL);

	switcher_attach_back_gesture(entry_screen);
}

static void entry_open(const char *title, const char *initial, bool secret, void (*done)(const char *, void *),
					   void *user) {
	if (!entry_screen) {
		entry_build();
	}

	entry_done = done;
	entry_user = user;

	lv_label_set_text(entry_title, title);
	lv_textarea_set_password_mode(entry_field, secret);
	lv_textarea_set_text(entry_field, initial ? initial : "");
	keyboard_reset(entry_keyboard);
	keyboard_set_visible(entry_keyboard, true);

	// The second question of the hidden-network flow is asked on this same
	// page, and switching to the screen that is already up is not a no-op in
	// this player: the load hides and reshows it, and the touch indev stops
	// answering -- the page is then visible and dead. So the switch is only
	// made when it is a switch.
	bool switching = lv_screen_active() != entry_screen;
	if (switching) {
		switch_screen(entry_screen);
	}
}

// ---------------------------------------------------------------------------
// a network that does not broadcast its name
// ---------------------------------------------------------------------------

static char pending_ssid[WIFI_SSID_MAX];

static void hidden_password_entered(const char *text, void *user) {
	(void)user;
	// The passphrase is whatever was typed, including nothing: an open network
	// is joined the same way the page's own password page joins one, and the
	// connection's own failure says so if it turns out to be secured.
	wifi_connect(pending_ssid, text);
	switch_screen(wifisettings_screen);
}

static void hidden_ssid_entered(const char *text, void *user) {
	(void)user;

	size_t len = strlen(text);
	if (len == 0 || len > WIFINET_SSID_MAX) {
		// Still on the entry page: the field keeps what was typed, and the
		// toast says why it is not going anywhere.
		toast_error(tr("wifi_ssid_invalid"));
		return;
	}

	snprintf(pending_ssid, sizeof(pending_ssid), "%s", text);
	entry_open(pending_ssid, NULL, true, hidden_password_entered, NULL);
}

static void manual_row_cb(lv_event_t *e) {
	(void)e;
	entry_open(tr("wifi_hidden_network"), NULL, false, hidden_ssid_entered, NULL);
}

void wifinet_add_manual_row(lv_obj_t *container, gui_config_t *cfg) {
	page_cfg = cfg;
	settingsrow_action(container, "wifi_hidden_network", manual_row_cb, NULL);
}

// ---------------------------------------------------------------------------
// the resolver's addresses
// ---------------------------------------------------------------------------

static lv_obj_t *dns_row_value; // the summary on the Wi-Fi page
static lv_obj_t *dns_screen;    // and the page the two addresses live on
static lv_obj_t *dns_value[2];

static void dns_refresh(void) {
	char primary[NETDNS_ADDRESS_MAX];
	char secondary[NETDNS_ADDRESS_MAX];
	netdns_get(primary, secondary);

	// The Wi-Fi page shows where things stand in one line: an override, or the
	// router's first address, or nothing at all.
	if (dns_row_value) {
		lv_label_set_text(dns_row_value, primary[0] ? primary : tr("wifi_dns_not_set"));
	}
	if (dns_value[0] && dns_value[1]) {
		lv_label_set_text(dns_value[0], primary[0] ? primary : tr("wifi_dns_not_set"));
		lv_label_set_text(dns_value[1], secondary[0] ? secondary : tr("wifi_dns_not_set"));
	}
}

static void dns_entered(const char *text, void *user) {
	int slot = (int)(intptr_t)user;

	// Empty is a real answer -- "back to what the router hands out" -- and
	// anything else has to be an address the resolver could use. Both cases
	// stay on the page rather than closing it on a value that would be thrown
	// away afterwards.
	if (text[0] && !netdns_is_address(text)) {
		toast_error(tr("wifi_dns_invalid"));
		return;
	}

	char primary[NETDNS_ADDRESS_MAX];
	char secondary[NETDNS_ADDRESS_MAX];
	netdns_get(primary, secondary);

	if (slot == 0) {
		snprintf(primary, sizeof(primary), "%s", text);
	} else {
		snprintf(secondary, sizeof(secondary), "%s", text);
	}

	if (netdns_set(primary, secondary)) {
		toast_success(tr("wifi_dns_saved"));
	} else {
		toast_error(tr("wifi_dns_write_failed"));
	}

	// The page is still up: two rows that are not changed yet would be a lie
	// about what the resolver is now using.
	dns_refresh();
	switch_screen_return_to(dns_screen);
}

static void dns_row_cb(lv_event_t *e) {
	int slot = (int)(intptr_t)lv_event_get_user_data(e);

	char primary[NETDNS_ADDRESS_MAX];
	char secondary[NETDNS_ADDRESS_MAX];
	netdns_get(primary, secondary);

	entry_open(tr(slot == 0 ? "wifi_dns_primary" : "wifi_dns_secondary"), slot == 0 ? primary : secondary, false,
			   dns_entered, (void *)(intptr_t)slot);
}

static void dns_page_loaded_cb(lv_event_t *e) {
	(void)e;
	// The file is written by the DHCP client as well as by this page, so what
	// it says is only true as of the last time it was looked at.
	dns_refresh();
}

// Back to what the router hands out.
//
// An override is two strings in the config; clearing both is the whole of the
// reset, and `dns_entered` already treats an empty address as exactly that. The
// row is one tap rather than two because the empty-address route asks the user
// to know that an empty field is the answer, which nobody does.
static void dns_reset_cb(lv_event_t *e) {
	(void)e;
	if (netdns_set("", "")) {
		toast_success(tr("wifi_dns_saved"));
	} else {
		toast_error(tr("wifi_dns_write_failed"));
	}
	dns_refresh();
}

static void dns_open(void) {
	if (!dns_screen) {
		dns_screen = lv_obj_create(NULL);
		lv_obj_add_style(dns_screen, &theme_style_screen, 0);

		lv_obj_t *page = settingsrow_page(dns_screen, page_cfg, "wifi_dns_section");
		settingsrow_add(page, "wifi_dns_primary", &dns_value[0], dns_row_cb, (void *)(intptr_t)0);
		settingsrow_add(page, "wifi_dns_secondary", &dns_value[1], dns_row_cb, (void *)(intptr_t)1);
		// One tap back to the router's own addresses, and the row that does it
		// has no chevron because it opens nothing.
		settingsrow_action(page, "wifi_dns_reset", dns_reset_cb, NULL);

		lv_obj_add_event_cb(dns_screen, dns_page_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
		switcher_attach_back_gesture(dns_screen);
	}

	dns_refresh();
	switch_screen(dns_screen);
}

static void dns_summary_cb(lv_event_t *e) {
	(void)e;
	dns_open();
}

void wifinet_add_dns_row(lv_obj_t *container, gui_config_t *cfg) {
	page_cfg = cfg;

	// One row on the Wi-Fi page rather than two: the page is already taller
	// than this panel -- the switch, the network being used, the list and the
	// heading fill 480 px -- and it does not scroll. The two addresses get a
	// page of their own, opened the way every other row opens one.
	settingsrow_add(container, "wifi_dns_section", &dns_row_value, dns_summary_cb, NULL);

	// So the summary is right when the page comes back into view, not only
	// when it was built.
	lv_obj_add_event_cb(lv_obj_get_screen(container), dns_page_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	dns_refresh();
}
