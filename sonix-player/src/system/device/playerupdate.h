#ifndef PLAYERUPDATE_H
#define PLAYERUPDATE_H

#include <stdbool.h>
#include <stddef.h>

// The player's half of the over-the-air update (docs/PLAYER-UPDATE.md).
//
// Two things live here, both called from main() before the player proper:
//
//   * `sonix_player --verify <file> <sha256>` -- the check the launcher makes
//     before it touches /usr/bin/sonix_player. It lives in the player because
//     the player already carries sha256 and the board id, and because the
//     launcher is freestanding: teaching it to hash would mean a second copy of
//     the code that decides whether a binary is safe to install. The verifier
//     refuses a package for another board, a file that is not a 32-bit
//     little-endian MIPS ELF, and a digest that does not match.
//
//   * `playerupdate_mark_healthy()` -- the mark a freshly installed player
//     leaves once it is running, which is what tells the launcher that the swap
//     worked. Written only when /data/update.lock exists: without a lock there
//     is no update in flight and no reason to write anything.
bool playerupdate_should_run(int argc, char **argv);

// Returns the process exit status: 0 verified, non-zero refused.
int playerupdate_verify_main(int argc, char **argv);

// Called once the player is up (before the event loop). Writes /data/update.ok
// when an update is in flight and logs the version that is now running.
void playerupdate_mark_healthy(void);

// ---------------------------------------------------------------------------
// The package on the card
// ---------------------------------------------------------------------------

typedef enum {
	PLAYERUPDATE_OK = 0,	  // staged: the next boot installs it
	PLAYERUPDATE_ASK,		  // staged, but the version has no order: ask the user
	PLAYERUPDATE_SAME,		  // nothing to do
	PLAYERUPDATE_OLDER,		  // refused
	PLAYERUPDATE_WRONG_BOARD, // a package for another device
	PLAYERUPDATE_NEEDS_IMAGE, // the package needs a newer firmware image than this one
	PLAYERUPDATE_NO_CARD,
	PLAYERUPDATE_NOT_A_PACKAGE, // unreadable, malformed, or the digest does not match
} playerupdate_result_t;

// Verifies the package and, when it passes, writes <card>/sonix/{player.new,
// player.sha256, pending}: everything the launcher needs and nothing more. The
// reboot is the caller's -- this only prepares the card.
playerupdate_result_t playerupdate_install(const char *package_path, char *why, size_t why_n);

// The package to offer: the board's own name at the card's root, the generic
// name, then any .tar under <card>/sonix/. False when there is nothing to offer.
bool playerupdate_find_package(char *out, size_t out_size);

// ---------------------------------------------------------------------------
// The network path: the release's index.json, then the package it names
// ---------------------------------------------------------------------------

typedef struct {
	char version[64];
	char package[96];
	char url[320];
	char sha256[65];
	char notes[2048]; // the tag message, rendered by the update card
	long size;
} playerupdate_remote_t;

// Where the index is: [update] index_url, else the compiled-in default
// (PLAYERUPDATE_INDEX_URL, this repository's newest release).
bool playerupdate_remote_index_url(char *out, size_t size);

// Fetches the index and picks this device's entry. Blocking (network): call it
// from a worker thread, never from the UI thread. False with a sentence in `why`.
bool playerupdate_remote_check(playerupdate_remote_t *out, char *why, size_t why_n);

// Downloads the package the index names, holds the archive to the index's digest
// and installs it. Blocking: a worker thread again.
playerupdate_result_t playerupdate_remote_install(const playerupdate_remote_t *remote, char *why, size_t why_n);

// Progress of that download, polled by the card while the worker runs.
void playerupdate_progress(long *done, long *total);

// Cancels it; the worker notices between chunks.
void playerupdate_cancel(void);

// "ready", "older", ... -- for the log and for --install-package's exit status.
const char *playerupdate_verdict_name(playerupdate_result_t result);

#endif /* PLAYERUPDATE_H */
