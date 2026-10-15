#ifndef CARDROOT_H
#define CARDROOT_H

#include <stdbool.h>
#include <stddef.h>

#include "src/system/db/sqlite3.h"

// The card root an index database is filed under.
//
// The card has more than one possible name on this firmware (/mnt/sd_0,
// /data/mnt/sd_0, /usr/data/mnt/sd_0 -- see storage_sd_root_aliases()) and the
// player mounts it under whichever one it resolves at startup. They are not
// always symlinks to one directory: /data/mnt/sd_0 is a real directory on the
// internal UBIFS, and the Wi-Fi transfer page bind-mounts the card there, so
// the same card can be reachable by two names at once. A row in MEDIA_TABLE is
// an absolute path, so a scan running under one name does not recognise rows
// filed under another: it reads every file as new and inserts a second copy of
// it -- the library with every track twice.
//
// The index remembers the root it was written under (CARD_ROOT, one row) and,
// when it is opened under another one, moves its paths across. Rows that meet on
// the same file are one row again, which is also what clears a library that
// already has its duplicates: a database with no recorded root at all -- every
// one written before this existed -- has every known name rewritten to the one
// in use.
//
// Moves the paths in `db` from the root it was filed under, and from any of
// `aliases` (the other names the card can have, which are used only when
// `new_root` is itself one of them), to `new_root`. Rows that collide are
// deduplicated. Records `new_root` as the root in use. Returns the number of
// older names moved across, which is 0 when there was nothing to do.
//
// The caller owns locking and must not hold an open transaction that it wants
// to keep: the move runs inside a savepoint of its own.
int card_root_reconcile(sqlite3 *db, const char *new_root, const char *const *aliases, size_t alias_count);

// The root a database path belongs to: the index always lives at
// <root>/.local/library.db. False when the path is not shaped that way.
bool card_root_of_db_path(const char *db_path, char *out, size_t size);

#endif
