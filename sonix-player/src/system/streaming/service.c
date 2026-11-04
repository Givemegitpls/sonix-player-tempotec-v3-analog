#include "service.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "src/system/core/utils.h"

// The registry. Two tables rather than one because they answer different
// questions at different times (see service.h):
//
//   declared[]  what the manifests say: the path prefixes and the cache
//               directory. Filled by the addon scan, valid as soon as the card
//               is read, no library opened.
//   open[]      the sonix_addon_service_t of each library that has been opened.
//               Everything about the account, plus the exact cache hooks.
//
// The read side runs on any thread and never takes a lock: both tables are
// filled on the UI thread before the pages that use them exist, and a service is
// only ever appended (a card change rewrites declared[] under the same rules the
// addon registry uses -- on the UI thread, before anything reads it again).
#define DECLARED_MAX SERVICE_MAX

typedef struct {
	char id[32];
	char prefixes[SERVICE_PREFIX_MAX][96];
	int prefix_count;
	char cache_rel[96];
	bool used;
} declared_t;

static declared_t declared[DECLARED_MAX];
static int declared_count;

static const sonix_addon_service_t *open_services[SERVICE_MAX];
static int open_count;

static char card_root[256];

void service_set_root(const char *sd_root) {
	if (sd_root && sd_root == card_root) {
		return; // called with our own buffer: nothing to copy
	}
	snprintf(card_root, sizeof(card_root), "%s", sd_root ? sd_root : "");
	for (int i = 0; i < open_count; i++) {
		if (open_services[i]->set_root) {
			open_services[i]->set_root(card_root);
		}
	}
}

void service_set_root_all(void) { service_set_root(card_root); }

static declared_t *declared_for(const char *id) {
	for (int i = 0; i < declared_count; i++) {
		if (strcmp(declared[i].id, id) == 0) {
			return &declared[i];
		}
	}
	return NULL;
}

void service_register_paths(const char *id, const char *const *prefixes, int count, const char *cache_rel) {
	if (!id || !id[0]) {
		return;
	}
	declared_t *slot = declared_for(id);
	if (!slot) {
		if (declared_count >= DECLARED_MAX) {
			fprintf(stderr, "service: no room for %s (raise SERVICE_MAX)\n", id);
			return;
		}
		slot = &declared[declared_count++];
		memset(slot, 0, sizeof(*slot));
		snprintf(slot->id, sizeof(slot->id), "%s", id);
	}
	slot->prefix_count = 0;
	for (int i = 0; i < count && i < SERVICE_PREFIX_MAX; i++) {
		const char *prefix = prefixes ? prefixes[i] : NULL;
		if (!prefix || !prefix[0]) {
			continue;
		}
		// A leading slash would make the prefix absolute, and the card moves.
		snprintf(slot->prefixes[slot->prefix_count++], sizeof(slot->prefixes[0]), "%s",
				 prefix[0] == '/' ? prefix + 1 : prefix);
	}
	snprintf(slot->cache_rel, sizeof(slot->cache_rel), "%s",
			 cache_rel && cache_rel[0] ? (cache_rel[0] == '/' ? cache_rel + 1 : cache_rel) : "");
	slot->used = true;
}

// The card is gone: the manifests that declared those paths went with it. The
// next scan declares them again, so nothing would be forgotten if this were
// skipped -- what it fixes is the window in between, where the player would
// still call a path under the old mount its own.
void service_forget_declared(void) {
	memset(declared, 0, sizeof(declared));
	declared_count = 0;
}

void service_register(const sonix_addon_service_t *service) {
	if (!service || !service->id) {
		return;
	}
	for (int i = 0; i < open_count; i++) {
		if (open_services[i] == service || (open_services[i]->id && strcmp(open_services[i]->id, service->id) == 0)) {
			open_services[i] = service; // opened twice, or replaced: keep one slot
			return;
		}
	}
	if (open_count >= SERVICE_MAX) {
		fprintf(stderr, "service: no room for %s (raise SERVICE_MAX)\n", service->id);
		return;
	}
	open_services[open_count++] = service;
	printf("service: %s registered\n", service->id);
}

int service_count(void) { return open_count; }

const sonix_addon_service_t *service_at(int index) {
	return index >= 0 && index < open_count ? open_services[index] : NULL;
}

void service_unregister(const char *id) {
	if (!id || !id[0]) {
		return;
	}
	for (int i = 0; i < open_count; i++) {
		if (!open_services[i]->id || strcmp(open_services[i]->id, id) != 0) {
			continue;
		}
		// Shift rather than swap: service_at() is walked by the pages in
		// registration order and nothing depends on the order staying still, but
		// a table that moves under a loop is a trap for the next reader.
		for (int j = i; j + 1 < open_count; j++) {
			open_services[j] = open_services[j + 1];
		}
		open_count--;
		open_services[open_count] = NULL;
		printf("service: %s unregistered\n", id);
		return;
	}
}

static const sonix_addon_service_t *open_for_id(const char *id) {
	for (int i = 0; i < open_count; i++) {
		if (open_services[i]->id && id && strcmp(open_services[i]->id, id) == 0) {
			return open_services[i];
		}
	}
	return NULL;
}

// Which declaration owns `path`, by prefix. NULL when none does.
static const declared_t *declared_owning(const char *path) {
	if (!path || !path[0] || !card_root[0]) {
		return NULL;
	}
	char full[sizeof(declared[0].prefixes[0]) + sizeof(card_root)];
	for (int i = 0; i < declared_count; i++) {
		for (int p = 0; p < declared[i].prefix_count; p++) {
			snprintf(full, sizeof(full), "%s/%s", card_root, declared[i].prefixes[p]);
			if (strncmp(path, full, strlen(full)) == 0) {
				return &declared[i];
			}
		}
	}
	return NULL;
}

const sonix_addon_service_t *service_for_path(const char *path) {
	// An open library answers for itself first: its owns_path knows about
	// symlinks and case where a prefix does not.
	for (int i = 0; i < open_count; i++) {
		if (open_services[i]->owns_path && open_services[i]->owns_path(path)) {
			return open_services[i];
		}
	}
	const declared_t *decl = declared_owning(path);
	return decl ? open_for_id(decl->id) : NULL;
}

const sonix_addon_service_t *service_for_track(const char *path) {
	const sonix_addon_service_t *service = service_for_path(path);
	return service && service->track_id && service->track_id(path) > 0 ? service : NULL;
}

bool service_owns(const char *path) { return declared_owning(path) != NULL || service_for_path(path) != NULL; }

const char *service_id_for_path(const char *path) {
	const sonix_addon_service_t *service = service_for_path(path);
	if (service) {
		return service->id;
	}
	const declared_t *decl = declared_owning(path);
	return decl ? decl->id : NULL;
}

const char *service_cache_dir(const char *id) {
	const sonix_addon_service_t *service = open_for_id(id);
	if (service && service->cache_dir) {
		const char *dir = service->cache_dir();
		if (dir && dir[0]) {
			return dir;
		}
	}
	static char dir[256];
	declared_t *decl = id ? declared_for(id) : NULL;
	if (!decl || !decl->cache_rel[0] || !card_root[0]) {
		return "";
	}
	snprintf(dir, sizeof(dir), "%s/%s", card_root, decl->cache_rel);
	return dir;
}

int service_track_id(const char *path) {
	const sonix_addon_service_t *service = service_for_path(path);
	return service && service->track_id ? service->track_id(path) : 0;
}

bool service_any_downloading(void) {
	for (int i = 0; i < open_count; i++) {
		if (open_services[i]->downloading && open_services[i]->downloading()) {
			return true;
		}
	}
	return false;
}

bool service_any_network_wanted(void) {
	for (int i = 0; i < open_count; i++) {
		if (open_services[i]->network_wanted && open_services[i]->network_wanted()) {
			return true;
		}
	}
	return false;
}

void service_clear_on_exit_all(void) {
	for (int i = 0; i < open_count; i++) {
		if (open_services[i]->clear_on_exit) {
			open_services[i]->clear_on_exit();
		}
	}
}

// A directory and what is in it. Used for the manifest's cache_path, the
// fallback when the library was never opened: a factory reset has to empty the
// cache whether or not the service ran this session.
static void remove_tree(const char *dir) {
	DIR *d = opendir(dir);
	if (!d) {
		return;
	}
	struct dirent *e;
	while ((e = readdir(d)) != NULL) {
		if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
			continue;
		}
		char path[512];
		if (snprintf(path, sizeof(path), "%s/%s", dir, e->d_name) >= (int)sizeof(path)) {
			continue;
		}
		struct stat st;
		if (lstat(path, &st) != 0) {
			continue;
		}
		if (S_ISDIR(st.st_mode)) {
			remove_tree(path);
			rmdir(path);
		} else {
			remove(path);
		}
	}
	closedir(d);
}

void service_wipe_all(void) {
	for (int i = 0; i < open_count; i++) {
		if (open_services[i]->wipe) {
			open_services[i]->wipe();
		} else if (open_services[i]->clear_on_exit) {
			open_services[i]->clear_on_exit();
		}
	}
	// And whatever a service that was never opened -- or one that has no way of
	// emptying its own cache -- left behind. The check is on the *hook*, not on
	// the library being open: an open table with neither `wipe` nor
	// `clear_on_exit` (Spotify: a session blob the engine owns) would otherwise
	// keep its declared cache through a factory reset.
	for (int i = 0; i < declared_count; i++) {
		if (!declared[i].used || !declared[i].cache_rel[0] || !card_root[0]) {
			continue;
		}
		const sonix_addon_service_t *open = open_for_id(declared[i].id);
		if (open && (open->wipe || open->clear_on_exit)) {
			continue; // the library has already emptied it its own way
		}
		char dir[sizeof(declared[i].cache_rel) + sizeof(card_root)];
		snprintf(dir, sizeof(dir), "%s/%s", card_root, declared[i].cache_rel);
		remove_tree(dir);
	}
}
