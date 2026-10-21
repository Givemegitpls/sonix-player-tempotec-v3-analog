#ifndef STREAMKEYS_H
#define STREAMKEYS_H

#include <stdbool.h>

// The streaming services' application credentials, read from a file on the
// device rather than written in here.
//
// Qobuz and Tidal offer no way to talk to their APIs without a pair of
// application keys, and the ones this device has are HiBy's: they live inside
// /usr/bin/hiby_player, Tidal's even assembled byte by byte at run time so it
// never appears as a string. They are HiBy's keys, not this project's, and open
// source that carries them publishes them.
//
// So they sit in a file on the rootfs, which whoever assembles the firmware puts
// alongside the other resource files. It is written as an INI:
//
//     [qobuz]
//     app_id = ...
//     app_secret = ...
//
//     [tidal]
//     client_id = ...
//     client_secret = ...
//
//     [podcast]
//     api_key = ...
//     api_secret = ...
//
//     [lastfm]
//     api_key = ...
//     api_secret = ...
//
// and shipped sealed, as /usr/resource/sonix/components/streaming-keys.bin, by
// tools/seal_streamkeys.py. The key that opens it is not in the source: the
// tool keeps it in streaming-keys.key, outside git, and the Makefile compiles
// it into the player. That keeps the keys out of an unpacked image; it does not
// keep them from whoever takes the binary apart.
//
// A plain streaming-keys.ini next to it is still read when there is no .bin,
// for the simulator and for builds made without a key. Which of the two a file
// is, is decided by its first bytes, not its name.
//
// Without either, or without a section, the matching service disables itself
// and its menu entry says so instead of failing halfway through a login.
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
const char *streamkeys_qobuz_app_id(void);
const char *streamkeys_qobuz_app_secret(void);
const char *streamkeys_tidal_client_id(void);
const char *streamkeys_tidal_client_secret(void);
// Podcast Index. This pair identifies this player rather than a HiBy
// application: it is requested free on podcastindex.org, and each request is
// signed with the SHA-1 of key + secret + time. Same rule as the other two:
// out of the source, in a file whoever assembles the firmware fills in.
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
// The card's streaming.json
//
// The services' own sign-ins live on the card, not in the player's storage: the
// card is what carries an account from one player to the next, and a device
// handed on keeps nothing about anybody's account. Two files, under
// <card>/sonix/:
//
//   streaming.json           the user's. The player reads it and never writes it.
//   streaming-session.json   the player's. Written when a sign-in completes or a
//                            Tidal token is renewed; newer than the user's file
//                            and wins over it.
//
// streaming.json, every field optional, unknown keys ignored:
//
//   {
//     "qobuz": {"app_id": "...", "app_secret": "...", "token": "...", "user": "..."},
//     "tidal": {"access_token": "...", "refresh_token": "...", "expires_at": 0}
//   }
//
// The token is what a service actually needs; app_id is Qobuz's public
// application number and has a built-in default, and Tidal's client pair is
// built in the same way (see tidal.c).
void streamkeys_attach_card(const char *sd_root);

const char *streamkeys_card_json_path(void); // <card>/sonix/streaming.json, NULL without a card
const char *streamkeys_session_json_path(void);

const char *streamkeys_qobuz_token(void);
const char *streamkeys_qobuz_token_user(void);
const char *streamkeys_tidal_access_token(void);
const char *streamkeys_tidal_refresh_token(void);
long streamkeys_tidal_expires_at(void);

// What the player derived from Qobuz's web player the last time (see
// qobuzkey.h): the application id and the secret that its signed calls take,
// with when that happened. Kept in the session file, on the card -- nothing of
// this is in the image.
const char *streamkeys_qobuz_cached_app_id(void);
const char *streamkeys_qobuz_cached_app_secret(void);
long streamkeys_qobuz_creds_at(void);
void streamkeys_save_qobuz_creds(const char *app_id, const char *app_secret, long at);

// The player's own file: written atomically. An empty token is how a service is
// signed out -- the file keeps the other service's session.
void streamkeys_save_qobuz_session(const char *token, const char *user);
void streamkeys_save_tidal_session(const char *access_token, const char *refresh_token, long expires_at);

#endif /* STREAMKEYS_H */
