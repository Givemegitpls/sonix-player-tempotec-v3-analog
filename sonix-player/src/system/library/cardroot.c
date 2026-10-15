#include "cardroot.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

// The tables that hold a path into the card. Those whose primary key is the
// path need UPDATE OR REPLACE: rewriting both /mnt/sd_0/x and /data/mnt/sd_0/x
// to the same name would otherwise collide.
static const struct {
	const char *table;
	const char *column;
	bool key;
} CARD_PATH_COLUMNS[] = {
	{"MEDIA_TABLE", "path", false},
	{"ARTIST_LINK", "path", false},
	{"ALBUM_ARTIST_LINK", "path", false},
	{"GENRE_LINK", "path", false},
	{"FOLDER_TABLE", "path", true},
	{"CUE_STATE", "path", true},
	{"PLAYLIST_COUNTS", "path", true},
	{"FAVOURITES", "path", true},
	{"PLAYBACK_STATE", "path", false},
	{"PLAYBACK_QUEUE", "path", false},
	{"PLAYBACK_QUEUE_QUERY", "value", false},
};

static bool run(sqlite3 *db, const char *sql) { return sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK; }

// Rewrites one column: a value that is `from` itself, or begins with `from`
// followed by '/', becomes the same value under `to`.
static void card_path_retarget(sqlite3 *db, const char *table, const char *column, bool key, const char *from,
							   const char *to) {
	// substr(col, ?2) is the "/rest" after the prefix and substr(col, 1, ?2 - 1)
	// is the prefix itself, so the match is anchored: it cannot take "sd_00" for
	// "sd_0". Written with substr rather than LIKE because LIKE would also treat
	// a '_' in the name as a wildcard.
	char sql[320];
	snprintf(sql, sizeof(sql),
			 "UPDATE %s%s SET %s = ?1 || substr(%s, ?2)"
			 " WHERE %s = ?3 OR (substr(%s, 1, ?2 - 1) = ?3 AND substr(%s, ?2, 1) = '/')",
			 key ? "OR REPLACE " : "", table, column, column, column, column, column);
	sqlite3_stmt *stmt = NULL;
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
		fprintf(stderr, "library: cannot prepare the root move for %s.%s: %s\n", table, column, sqlite3_errmsg(db));
		return;
	}
	sqlite3_bind_text(stmt, 1, to, -1, SQLITE_STATIC);
	sqlite3_bind_int(stmt, 2, (int)strlen(from) + 1);
	sqlite3_bind_text(stmt, 3, from, -1, SQLITE_STATIC);
	sqlite3_step(stmt);
	sqlite3_finalize(stmt);
}

// Drops rows that became identical in the columns that identify them.
static void card_rows_dedupe(sqlite3 *db, const char *table, const char *columns) {
	char sql[192];
	snprintf(sql, sizeof(sql), "DELETE FROM %s WHERE rowid NOT IN (SELECT MIN(rowid) FROM %s GROUP BY %s)", table,
			 table, columns);
	run(db, sql);
}

// Whether any row of the index is filed under `prefix` (the prefix itself, or a
// path below it). MEDIA_TABLE is indexed by path, so this is a range read rather
// than a table scan.
static bool card_root_used(sqlite3 *db, const char *prefix) {
	char lo[PATH_MAX + 2], hi[PATH_MAX + 2];
	snprintf(lo, sizeof(lo), "%s/", prefix);
	snprintf(hi, sizeof(hi), "%s0", prefix); // '0' is the byte after '/'

	sqlite3_stmt *stmt = NULL;
	if (sqlite3_prepare_v2(db, "SELECT 1 FROM MEDIA_TABLE WHERE path = ?1 OR (path >= ?2 AND path < ?3) LIMIT 1", -1,
						   &stmt, NULL) != SQLITE_OK) {
		return false;
	}
	sqlite3_bind_text(stmt, 1, prefix, -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt, 2, lo, -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt, 3, hi, -1, SQLITE_STATIC);
	bool used = sqlite3_step(stmt) == SQLITE_ROW;
	sqlite3_finalize(stmt);
	return used;
}

// The root the index was last written under, if it was.
static bool card_root_get(sqlite3 *db, char *out, size_t size) {
	sqlite3_stmt *stmt = NULL;
	if (sqlite3_prepare_v2(db, "SELECT root FROM CARD_ROOT WHERE id = 0", -1, &stmt, NULL) != SQLITE_OK) {
		return false;
	}
	bool found = false;
	if (sqlite3_step(stmt) == SQLITE_ROW) {
		const char *root = (const char *)sqlite3_column_text(stmt, 0);
		if (root && root[0]) {
			snprintf(out, size, "%s", root);
			found = true;
		}
	}
	sqlite3_finalize(stmt);
	return found;
}

// Records `root` as the one the index is filed under.
static void card_root_put(sqlite3 *db, const char *root) {
	sqlite3_stmt *stmt = NULL;
	if (sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO CARD_ROOT(id, root) VALUES(0, ?1)", -1, &stmt, NULL) !=
		SQLITE_OK) {
		return;
	}
	sqlite3_bind_text(stmt, 1, root, -1, SQLITE_STATIC);
	sqlite3_step(stmt);
	sqlite3_finalize(stmt);
}

bool card_root_of_db_path(const char *db_path, char *out, size_t size) {
	static const char suffix[] = "/.local/library.db";
	size_t len = strlen(db_path);
	size_t suffix_len = sizeof(suffix) - 1;
	if (len <= suffix_len || strcmp(db_path + len - suffix_len, suffix) != 0) {
		return false;
	}
	size_t root_len = len - suffix_len;
	if (root_len == 0 || root_len >= size) {
		return false;
	}
	memcpy(out, db_path, root_len);
	out[root_len] = '\0';
	return true;
}

int card_root_reconcile(sqlite3 *db, const char *new_root, const char *const *aliases, size_t alias_count) {
	if (!db || !new_root || !new_root[0]) {
		return 0;
	}

	char stored[PATH_MAX];
	bool have_stored = card_root_get(db, stored, sizeof(stored));

	// The prefixes whose rows are the same files under another name. The stored
	// root is one of them; every other name the card can have is another, but
	// only when the root in use is itself one of those names -- on the host
	// build, and for the /tmp fallback of a read-only card, it is not.
	const char *from[8];
	size_t from_count = 0;
	if (have_stored && strcasecmp(stored, new_root) != 0) {
		from[from_count++] = stored;
	}

	bool new_root_is_alias = false;
	for (size_t i = 0; i < alias_count; i++) {
		new_root_is_alias |= strcasecmp(new_root, aliases[i]) == 0;
	}
	if (new_root_is_alias) {
		for (size_t i = 0; i < alias_count && from_count < sizeof(from) / sizeof(from[0]); i++) {
			if (strcasecmp(aliases[i], new_root) == 0) {
				continue;
			}
			bool known = false;
			for (size_t j = 0; j < from_count; j++) {
				known |= strcasecmp(from[j], aliases[i]) == 0;
			}
			if (!known && card_root_used(db, aliases[i])) {
				from[from_count++] = aliases[i];
			}
		}
	}

	if (from_count > 0) {
		run(db, "SAVEPOINT card_root");
		for (size_t i = 0; i < from_count; i++) {
			for (size_t c = 0; c < sizeof(CARD_PATH_COLUMNS) / sizeof(CARD_PATH_COLUMNS[0]); c++) {
				card_path_retarget(db, CARD_PATH_COLUMNS[c].table, CARD_PATH_COLUMNS[c].column,
								   CARD_PATH_COLUMNS[c].key, from[i], new_root);
			}
		}
		// The rows that met on the same file are one row again.
		card_rows_dedupe(db, "MEDIA_TABLE", "path, cue_id");
		card_rows_dedupe(db, "ARTIST_LINK", "path, artist");
		card_rows_dedupe(db, "ALBUM_ARTIST_LINK", "path, album_artist");
		card_rows_dedupe(db, "GENRE_LINK", "path, genre");
		card_rows_dedupe(db, "PLAYBACK_QUEUE", "path");
		run(db, "RELEASE card_root");
	}

	if (!have_stored || strcasecmp(stored, new_root) != 0) {
		card_root_put(db, new_root);
	}

	return (int)from_count;
}
