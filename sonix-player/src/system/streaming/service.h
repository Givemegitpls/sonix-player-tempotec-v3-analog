#ifndef STREAM_SERVICE_H
#define STREAM_SERVICE_H

#include <stdbool.h>
#include <stddef.h>

#include "src/addon/addon_service.h"

// The streaming services that hand the player files on the card -- Qobuz,
// Tidal, and the two that came from the rekkod fork -- seen from the rest of the
// program.
//
// Every one of them works the same way from the outside: a track is a file under
// `.local/<service>-cache/` on the card, and the path is its identity (the
// service can always map a path back to its own track id). What differs is what
// happens behind that: whose account the star writes to, which playlists "Add to
// playlist" offers, which page "Show album" opens.
//
// Before this table every place that cared -- the player's star and badge, the
// control centre, the playlist picker, the track menu, power management, the
// card watcher, every exit path -- carried its own "if Qobuz ... else if Tidal"
// chain, and adding a service meant finding all of them. Now a service fills in
// one sonix_addon_service_t and registers it; those places loop over the
// registry. The shape of that table is the addon interface
// (src/addon/addon_service.h) because the services are addons on the card: this
// module is the player's half of it.
//
// Two sources, on purpose:
//
//   * service_register_paths(), fed from the addon manifests at scan time. It
//     answers "is this path a service's" from the path alone, with no library
//     opened -- which is what USB export, the sleep timer and the factory reset
//     need, and they ask during boot.
//   * service_register(), fed by a library once it is open (its
//     sonix_addon_service_entry), for everything about the account.
//
// Threads: the cache hooks (owns_path, downloading, network_wanted, ...) are
// safe from any thread; the account hooks are called on the UI thread and their
// `done` arrives on the service's worker, which bounces it with gui_post().

#define SERVICE_MAX 8
#define SERVICE_PREFIX_MAX 4

// The card: every path in a manifest is relative to it. Called at start-up and
// on every card change, before the caches are touched.
void service_set_root(const char *sd_root);

// A manifest's declaration, from the addon scan. `prefixes` are relative to the
// card ("/.local/qobuz-cache/" is written as ".local/qobuz-cache/"); `cache_rel`
// is the directory a factory reset empties, or NULL.
void service_register_paths(const char *id, const char *const *prefixes, int count, const char *cache_rel);

// The card went away: forget what the manifests declared (the paths, and the
// cache directories a factory reset empties). Called from card_databases_detach();
// the next addon scan declares them again.
void service_forget_declared(void);

// The library is open: from here on the account questions have answers.
void service_register(const sonix_addon_service_t *service);

// The library is going away (addon unloaded, card pulled): drop its entry, so
// that the player stops asking a table that lives in a library it is about to
// dlclose(). Matches by id, which is what a reload keeps. Declared paths are
// forgotten by service_forget_declared() and not here: this is the code half.
void service_unregister(const char *id);

int service_count(void);
const sonix_addon_service_t *service_at(int index);

// The service whose cache holds `path` and which is open, or NULL. Use the
// _owns/_id forms below when the answer has to be right before the library was
// opened.
const sonix_addon_service_t *service_for_path(const char *path);

// Like service_for_path(), but only for an actual track of the service: what
// the star and "add to playlist" should write to. NULL for a sidecar, for an
// artwork file, and for everything that is not a service's at all.
const sonix_addon_service_t *service_for_track(const char *path);

// True when any service -- declared or open -- owns `path`.
bool service_owns(const char *path);

// The id of the service that owns `path` ("qobuz", "tidal"), or NULL. This is
// what the "Source" row of the track details and the player's badge read.
const char *service_id_for_path(const char *path);

// The cache directory of the service with that id, "" when it has none or the
// card is not mounted. Answered from the library when it is open, from the
// manifest prefix when it is not: main.c forgets library rows under these at
// boot, before any addon page exists.
const char *service_cache_dir(const char *id);

// The owning service's track id for `path`, 0 when it is not one of its tracks
// (a sidecar, an artwork file) or no service owns it. Needs the library: the
// path alone says which service, not which track.
int service_track_id(const char *path);

// True when some open service is downloading, or (with `wanted`) needs the
// network anyway. The sleep timer and the power manager ask these.
bool service_any_downloading(void);
bool service_any_network_wanted(void);

// The old *_set_root() calls, in one place. `service_set_root` above covers the
// manifests; this repeats it on the open libraries.
void service_set_root_all(void);

// Shutdown, restart, firmware update, factory reset -- the four exit paths that
// used to call qobuzcache_clear_on_exit() and tidalcache_clear_on_exit() by name.
void service_clear_on_exit_all(void);
void service_wipe_all(void);

#endif /* STREAM_SERVICE_H */
