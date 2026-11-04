#ifndef STREAMKEYS_H
#define STREAMKEYS_H

#include <stdbool.h>
#include <stddef.h>

// The application credentials of the two streaming services that are part of
// the binary: Podcast Index and Last.fm.
//
// They are read from a file on the device rather than written in here. Podcast
// Index and Last.fm have no other way in -- each is an API account requested in
// this player's name, and every call is signed with its secret -- so the file
// is what makes them work at all; without it they disable themselves and their
// menu entry says so instead of failing halfway through a request.
//
// Qobuz, Tidal, Yandex Music and Zvuk are *not* here. Their keys and their
// sessions belong to the addons that use them, in
// <card>/Addons/<id>/settings.ini and <card>/Addons/<id>/session.ini, because
// the player knows nothing about them any more (docs/ADDONS.md, "Where an addon
// keeps its settings"). Nothing in this file is compiled in for them and
// nothing of theirs is published to them.
//
// So the file, on the rootfs, alongside the other resource files, is an INI:
//
//     [podcast]
//     api_key = ...
//     api_secret = ...
//
//     [lastfm]
//     api_key = ...
//     api_secret = ...
//
// and may be shipped sealed, as
// /usr/resource/sonix/components/streaming-keys.bin, by tools/seal_streamkeys.py;
// this port ships no `.bin`. The key that opens a sealed file is not in the
// source: the tool keeps it in streaming-keys.key, outside git, and the Makefile
// compiles it into the player. That keeps the keys out of an unpacked image; it
// does not keep them from whoever takes the binary apart.
//
// A plain streaming-keys.ini next to it is still read when there is no .bin,
// for the simulator and for builds made without a key. Which of the two a file
// is, is decided by its first bytes, not its name.
//
// The path can be changed with [streaming] keys_file in device_config.ini, to
// keep the keys on the card instead of the read-only rootfs -- the easy path
// the day a service changes its keys.

#include "src/system/core/respath.h"

#define STREAMKEYS_PATH SONIX_RESOURCE_DIR "/components/streaming-keys.bin"
#define STREAMKEYS_PLAIN_PATH SONIX_RESOURCE_DIR "/components/streaming-keys.ini"

// Reads the file. Call once at startup, after config_init(). A missing file is
// not an error.
void streamkeys_init(void);

// NULL when absent. The pointers stay valid for the life of the program.
//
// Podcast Index. This pair identifies this player rather than a HiBy
// application: it is requested free on podcastindex.org, and each request is
// signed with the SHA-1 of key + secret + time.
const char *streamkeys_podcast_key(void);
const char *streamkeys_podcast_secret(void);
// Last.fm. An API account of this player, requested on last.fm/api: it signs
// every call with the MD5 of the sorted parameters plus the secret.
const char *streamkeys_lastfm_key(void);
const char *streamkeys_lastfm_secret(void);

// Where they were read from, for the log and the information page. NULL when
// none were found.
const char *streamkeys_source(void);

// ---------------------------------------------------------------------------
// TEMPORARY: the migration of a service's keys out of this file
//
// The keys of Qobuz, Tidal, Yandex Music and Zvuk used to be sections of this
// same file, and the addons read them through streamkeys_*. They read their own
// files now, and this is the one function left that knows the file at all: an
// addon calls it once, on its first run, to carry whatever the owner had put in
// streaming-keys.ini (or in the sealed .bin, which no addon can open itself)
// into <card>/Addons/<id>/settings.ini.
//
// `section` and `key` are the addon's to name -- this file has no list of them
// and no idea which services exist. False when the file, the section or the key
// is not there; `out` is then empty. Remove this function, and the callers in
// addons/qobuz and addons/tidal, once a release has carried the keys.
bool streamkeys_legacy_get(const char *section, const char *key, char *out, size_t size);

#endif /* STREAMKEYS_H */
