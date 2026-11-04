#ifndef ADDON_SERVICE_H
#define ADDON_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The service half of an addon's interface, kept in a header of its own so that
// the layer that asks these questions -- streaming, the power manager, USB --
// needs no LVGL and no interface headers to do it. The two fields that are
// interface objects (the badge, the album page) are void pointers here and are
// LVGL's again on the other side.
//
// docs/ADDONS.md, "A service rather than an app", says what the player does with
// each of these and when it is asked.

typedef void (*sonix_addon_service_done_cb)(bool ok, const char *error, void *user);

// One of the account's playlists, as the "Add to playlist" picker shows it. The
// id is the service's own, as text: Qobuz numbers, Tidal UUIDs.
typedef struct {
	char id[64];
	char name[128];
	int track_count;
} sonix_addon_playlist_t;

typedef struct {
	// "qobuz": the log line, and the "Source" row of the track details.
	const char *id;

	// --- the cache, and the files it owns (any thread) ---------------------

	// Called at start-up and on every card change; NULL or "" disables the cache.
	void (*set_root)(const char *sd_root);
	const char *(*cache_dir)(void);
	bool (*owns_path)(const char *path);
	// The service's own id for a file of its cache, 0 when it is not a track
	// (a sidecar, an artwork file) or not this service's.
	int (*track_id)(const char *path);

	// A download is in progress right now; the device must not sleep.
	bool (*downloading)(void);
	// The service needs the network even with nothing downloading this instant
	// (between a track's end and the next one's first bytes).
	bool (*network_wanted)(void);
	// Shutdown, restart, firmware update: what of the cache must not survive a
	// power cycle goes. NULL when the cache is meant to persist.
	void (*clear_on_exit)(void);
	// Factory reset: everything goes, persistent or not. NULL means clear_on_exit.
	void (*wipe)(void);

	// --- the account (UI thread, `done` on the worker) ---------------------

	bool (*fav_get)(const char *path);
	// Fetches the mirror if it is not there yet; `done` arrives only when
	// something changed.
	void (*fav_refresh)(void (*done)(void));
	void (*fav_toggle)(const char *path, bool want, sonix_addon_service_done_cb done, void *user);
	bool (*album_id)(const char *path, char *out, size_t size);

	bool (*playlists_known)(void);
	void (*playlists_refresh)(void (*done)(void));
	int (*playlists)(sonix_addon_playlist_t *out, int max);
	void (*playlist_add)(const char *playlist_id, const char *path, sonix_addon_service_done_cb done, void *user);
	void (*playlist_create_with)(const char *name, const char *path, sonix_addon_service_done_cb done, void *user);

	// --- what only the GUI can supply --------------------------------------

	// The mark drawn over the artwork while one of this service's tracks plays.
	const void *badge; // an lv_image_dsc_t, or NULL
	// "Show album" from the player: opens the service's album page and returns
	// the screen back should lead to, or NULL when it could not.
	void *(*open_album)(const char *album_id, const char *title); // an lv_obj_t *
} sonix_addon_service_t;

// Exported by a service addon in addition to sonix_addon_entry(). NULL (or a
// NULL result) means "an app": nothing to register.
typedef const sonix_addon_service_t *(*sonix_addon_service_entry_fn)(uint32_t api);

// The symbol the loader looks for, so that forgetting the spelling is not a way
// to ship a service that silently stays an app.
#define SONIX_ADDON_SERVICE_ENTRY "sonix_addon_service_entry"

#endif /* ADDON_SERVICE_H */
