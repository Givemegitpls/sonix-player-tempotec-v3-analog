#include "streamkeys.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "build_gen/streamkeys_key.h"
#include "src/system/core/config.h"
#include "src/system/core/json.h"
#include "src/system/core/sha1.h"

#define STREAMKEYS_MAX_FILE 16384 // mirrored as MAX_FILE in the tool

#define SEAL_MAGIC "SXK1"
#define SEAL_MAGIC_LEN 4
#define SEAL_KEY_LEN 32
#define SEAL_NONCE_LEN 16

// The values stay here for the life of the program: short strings, read once,
// and every caller expects a pointer that does not die under it.
static char qobuz_app_id[64];
static char qobuz_app_secret[128];
static char tidal_client_id[64];
static char tidal_client_secret[128];
static char podcast_key[64];
static char podcast_secret[128];
static char lastfm_key[64];
static char lastfm_secret[64];
static char source_path[256];
static bool loaded_any;

// Not a plain memset: one on a buffer about to be freed may be dropped by the
// compiler.
static void wipe(void *p, size_t n) {
	volatile unsigned char *v = p;
	while (n--) {
		*v++ = 0;
	}
}

static void trim(char *s) {
	char *start = s;
	while (*start == ' ' || *start == '\t') {
		start++;
	}
	if (start != s) {
		memmove(s, start, strlen(start) + 1);
	}
	size_t len = strlen(s);
	while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\r' || s[len - 1] == '\n')) {
		s[--len] = '\0';
	}
}

// A tiny INI reader rather than config.c, for two reasons: this file is
// read-only and must not join what the player rewrites, and a failure here must
// not be able to corrupt device_config.ini.
//
// One line of the INI, already trimmed. `section` carries across lines.
static void parse_line(char *line, char section[32]) {
	if (line[0] == '\0' || line[0] == '#' || line[0] == ';') {
		return;
	}

	if (line[0] == '[') {
		char *close = strchr(line, ']');
		if (close) {
			*close = '\0';
			// Clamped to the buffer width: a section name half a kilobyte long
			// is not a section of interest, and the truncation is written out
			// rather than left to happen.
			snprintf(section, 32, "%.31s", line + 1);
			trim(section);
		}
		return;
	}

	char *eq = strchr(line, '=');
	if (!eq) {
		return;
	}
	*eq = '\0';
	char *key = line;
	char *value = eq + 1;
	trim(key);
	trim(value);
	if (value[0] == '\0') {
		return;
	}

	char *dest = NULL;
	size_t dest_size = 0;
	if (strcmp(section, "qobuz") == 0) {
		if (strcmp(key, "app_id") == 0) {
			dest = qobuz_app_id;
			dest_size = sizeof(qobuz_app_id);
		} else if (strcmp(key, "app_secret") == 0) {
			dest = qobuz_app_secret;
			dest_size = sizeof(qobuz_app_secret);
		}
	} else if (strcmp(section, "podcast") == 0) {
		if (strcmp(key, "api_key") == 0) {
			dest = podcast_key;
			dest_size = sizeof(podcast_key);
		} else if (strcmp(key, "api_secret") == 0) {
			dest = podcast_secret;
			dest_size = sizeof(podcast_secret);
		}
	} else if (strcmp(section, "lastfm") == 0) {
		if (strcmp(key, "api_key") == 0) {
			dest = lastfm_key;
			dest_size = sizeof(lastfm_key);
		} else if (strcmp(key, "api_secret") == 0) {
			dest = lastfm_secret;
			dest_size = sizeof(lastfm_secret);
		}
	} else if (strcmp(section, "tidal") == 0) {
		if (strcmp(key, "client_id") == 0) {
			dest = tidal_client_id;
			dest_size = sizeof(tidal_client_id);
		} else if (strcmp(key, "client_secret") == 0) {
			dest = tidal_client_secret;
			dest_size = sizeof(tidal_client_secret);
		}
	}

	if (dest) {
		snprintf(dest, dest_size, "%s", value);
		loaded_any = true;
	}
}

// Each line is copied out and wiped after use, so the text itself is left
// whole for the caller to wipe.
static void parse_text(char *text, size_t len) {
	char section[32] = "";
	char *p = text;
	char *end = text + len;
	while (p < end) {
		char *nl = memchr(p, '\n', (size_t)(end - p));
		char *stop = nl ? nl : end;
		char line[512];
		size_t n = (size_t)(stop - p);
		if (n >= sizeof(line)) {
			n = sizeof(line) - 1;
		}
		memcpy(line, p, n);
		line[n] = '\0';
		trim(line);
		parse_line(line, section);
		wipe(line, sizeof(line));
		p = stop + 1;
	}
}

#if STREAMKEYS_KEY_LEN == SEAL_KEY_LEN
// SHA1(key | nonce | block as 32-bit big-endian), XORed over `buf`.
static void apply_keystream(const unsigned char *key, const unsigned char *nonce, unsigned char *buf,
							size_t len) {
	unsigned char in[SEAL_KEY_LEN + SEAL_NONCE_LEN + 4];
	unsigned char block[SHA1_DIGEST_LEN];
	memcpy(in, key, SEAL_KEY_LEN);
	memcpy(in + SEAL_KEY_LEN, nonce, SEAL_NONCE_LEN);
	for (size_t off = 0, i = 0; off < len; off += SHA1_DIGEST_LEN, i++) {
		unsigned char *ctr = in + SEAL_KEY_LEN + SEAL_NONCE_LEN;
		ctr[0] = (unsigned char)(i >> 24);
		ctr[1] = (unsigned char)(i >> 16);
		ctr[2] = (unsigned char)(i >> 8);
		ctr[3] = (unsigned char)i;
		sha1(in, sizeof(in), block);
		size_t n = len - off < SHA1_DIGEST_LEN ? len - off : SHA1_DIGEST_LEN;
		for (size_t j = 0; j < n; j++) {
			buf[off + j] ^= block[j];
		}
	}
	wipe(in, sizeof(in));
	wipe(block, sizeof(block));
}
#endif

// The sealed form, laid out in tools/seal_streamkeys.py. False, with the reason
// in the log, when this build cannot open it.
static bool parse_sealed(const char *path, unsigned char *data, size_t len) {
#if STREAMKEYS_KEY_LEN == SEAL_KEY_LEN
	const size_t head = SEAL_MAGIC_LEN + SEAL_NONCE_LEN + SHA1_DIGEST_LEN;
	if (len < head) {
		printf("streamkeys: %s is cut short\n", path);
		return false;
	}
	const unsigned char *nonce = data + SEAL_MAGIC_LEN;
	const unsigned char *tag = nonce + SEAL_NONCE_LEN;
	unsigned char *text = data + head;
	size_t text_len = len - head;

	apply_keystream(streamkeys_key, nonce, text, text_len);

	unsigned char check[SHA1_DIGEST_LEN];
	unsigned char *signed_part = malloc(SEAL_KEY_LEN + SEAL_NONCE_LEN + text_len);
	if (!signed_part) {
		return false;
	}
	memcpy(signed_part, streamkeys_key, SEAL_KEY_LEN);
	memcpy(signed_part + SEAL_KEY_LEN, nonce, SEAL_NONCE_LEN);
	memcpy(signed_part + SEAL_KEY_LEN + SEAL_NONCE_LEN, text, text_len);
	sha1(signed_part, SEAL_KEY_LEN + SEAL_NONCE_LEN + text_len, check);
	wipe(signed_part, SEAL_KEY_LEN + SEAL_NONCE_LEN + text_len);
	free(signed_part);

	if (memcmp(check, tag, SHA1_DIGEST_LEN) != 0) {
		printf("streamkeys: %s was sealed with a key this build does not have\n", path);
		return false;
	}
	parse_text((char *)text, text_len);
	return true;
#else
	(void)data;
	(void)len;
	printf("streamkeys: %s is sealed and this build has no key to open it\n", path);
	return false;
#endif
}

// Reads `path`, sealed or plain. False when it is not there.
static bool load(const char *path) {
	FILE *f = fopen(path, "rb");
	if (!f) {
		return false;
	}
	unsigned char *data = malloc(STREAMKEYS_MAX_FILE + 1);
	if (!data) {
		fclose(f);
		return false;
	}
	size_t len = fread(data, 1, STREAMKEYS_MAX_FILE + 1, f);
	fclose(f);
	if (len > STREAMKEYS_MAX_FILE) {
		printf("streamkeys: %s is over %d bytes, ignored\n", path, STREAMKEYS_MAX_FILE);
		free(data);
		return true;
	}

	if (len >= SEAL_MAGIC_LEN && memcmp(data, SEAL_MAGIC, SEAL_MAGIC_LEN) == 0) {
		parse_sealed(path, data, len);
	} else {
		parse_text((char *)data, len);
	}
	wipe(data, len);
	free(data);
	return true;
}

void streamkeys_init(void) {
	const char *configured = config_get("streaming", "keys_file", NULL);
	const char *path = NULL;
	if (configured && configured[0]) {
		path = load(configured) ? configured : NULL;
	} else if (load(STREAMKEYS_PATH)) {
		path = STREAMKEYS_PATH;
	} else if (load(STREAMKEYS_PLAIN_PATH)) {
		path = STREAMKEYS_PLAIN_PATH;
	}

	if (!path) {
		printf("streamkeys: no %s, Tidal, Qobuz, podcasts and Last.fm stay off\n",
			   configured && configured[0] ? configured : STREAMKEYS_PATH);
		return;
	}

	snprintf(source_path, sizeof(source_path), "%s", path);

	// The log records that the keys are present, never their values: the log
	// lands on the card and from there goes to whoever asks for it about any
	// problem at all.
	printf("streamkeys: from %s -- Qobuz %s, Tidal %s, Podcast %s, Last.fm %s\n", path,
		   qobuz_app_id[0] && qobuz_app_secret[0] ? "yes" : "no",
		   tidal_client_id[0] && tidal_client_secret[0] ? "yes" : "no",
		   podcast_key[0] && podcast_secret[0] ? "yes" : "no",
		   lastfm_key[0] && lastfm_secret[0] ? "yes" : "no");
}

static const char *or_null(const char *s) { return s[0] ? s : NULL; }

const char *streamkeys_qobuz_app_id(void) { return or_null(qobuz_app_id); }
const char *streamkeys_qobuz_app_secret(void) { return or_null(qobuz_app_secret); }
const char *streamkeys_tidal_client_id(void) { return or_null(tidal_client_id); }
const char *streamkeys_tidal_client_secret(void) { return or_null(tidal_client_secret); }

const char *streamkeys_podcast_key(void) { return or_null(podcast_key); }
const char *streamkeys_podcast_secret(void) { return or_null(podcast_secret); }

const char *streamkeys_lastfm_key(void) { return or_null(lastfm_key); }
const char *streamkeys_lastfm_secret(void) { return or_null(lastfm_secret); }

const char *streamkeys_source(void) { return loaded_any ? source_path : NULL; }

// ---------------------------------------------------------------------------
// The card's streaming.json, and the session file the player writes beside it
// ---------------------------------------------------------------------------

#define CARD_DIR_NAME "sonix"
#define CARD_USER_FILE "streaming.json"
#define CARD_SESSION_FILE "streaming-session.json"
#define CARD_JSON_MAX 16384

// Sized for Tidal's tokens, which are eight-to-nine hundred characters each.
#define CARD_TOKEN_MAX 2048
#define CARD_NAME_MAX 128

static char card_user_path[512];
static char card_session_path[512];
static char qobuz_token[CARD_TOKEN_MAX];
static char qobuz_token_user[CARD_NAME_MAX];
static char tidal_access[CARD_TOKEN_MAX];
static char tidal_refresh[CARD_TOKEN_MAX];
static long tidal_expires;
// Derived from the web player, kept in the session file (qobuzkey.h).
static char cached_app_id[64];
static char cached_app_secret[128];
static long cached_creds_at;

const char *streamkeys_card_json_path(void) { return card_user_path[0] ? card_user_path : NULL; }
const char *streamkeys_session_json_path(void) { return card_session_path[0] ? card_session_path : NULL; }

const char *streamkeys_qobuz_token(void) { return or_null(qobuz_token); }
const char *streamkeys_qobuz_token_user(void) { return or_null(qobuz_token_user); }
const char *streamkeys_tidal_access_token(void) { return or_null(tidal_access); }
const char *streamkeys_tidal_refresh_token(void) { return or_null(tidal_refresh); }
long streamkeys_tidal_expires_at(void) { return tidal_expires; }

const char *streamkeys_qobuz_cached_app_id(void) { return or_null(cached_app_id); }
const char *streamkeys_qobuz_cached_app_secret(void) { return or_null(cached_app_secret); }
long streamkeys_qobuz_creds_at(void) { return cached_creds_at; }

// Copies a JSON string field out, and wipes whatever was there before: a token
// left behind in a buffer after it was replaced is a token still in memory.
static void take_str(const json_doc_t *doc, int obj, const char *key, char *out, size_t size) {
	char value[512];
	value[0] = '\0';
	if (obj < 0 || !json_obj_str(doc, obj, key, value, sizeof(value))) {
		return;
	}
	snprintf(out, size, "%s", value);
	wipe(value, sizeof(value));
}

// One file: `{"qobuz": {...}, "tidal": {...}}`. Unknown keys are ignored, and a
// field that is not there leaves the value it was given by the other file.
static bool read_card_json(const char *path, bool session) {
	FILE *f = fopen(path, "r");
	if (!f) {
		return false;
	}
	char *text = malloc(CARD_JSON_MAX + 1);
	if (!text) {
		fclose(f);
		return false;
	}
	size_t len = fread(text, 1, CARD_JSON_MAX + 1, f);
	fclose(f);
	if (len > CARD_JSON_MAX) {
		printf("streamkeys: %s is over %d bytes, ignored\n", path, CARD_JSON_MAX);
		free(text);
		return true;
	}
	text[len] = '\0';

	json_doc_t doc;
	if (!json_parse(text, &doc)) {
		printf("streamkeys: %s is not valid JSON, ignored\n", path);
		free(text);
		return true;
	}

	int root = json_root(&doc);
	int qobuz = json_get(&doc, root, "qobuz");
	int tidal = json_get(&doc, root, "tidal");

	take_str(&doc, qobuz, "token", qobuz_token, sizeof(qobuz_token));
	take_str(&doc, qobuz, "user", qobuz_token_user, sizeof(qobuz_token_user));
	if (session) {
		// Derived from the web player the last time, not the user's to write.
		take_str(&doc, qobuz, "app_id", cached_app_id, sizeof(cached_app_id));
		take_str(&doc, qobuz, "app_secret", cached_app_secret, sizeof(cached_app_secret));
		long at = json_obj_long(&doc, qobuz, "creds_at", -1);
		if (at >= 0) {
			cached_creds_at = at;
		}
	} else {
		// The user's file may name the application itself; the service derives
		// them from the web player when it does not.
		take_str(&doc, qobuz, "app_id", qobuz_app_id, sizeof(qobuz_app_id));
		take_str(&doc, qobuz, "app_secret", qobuz_app_secret, sizeof(qobuz_app_secret));
	}

	take_str(&doc, tidal, "access_token", tidal_access, sizeof(tidal_access));
	take_str(&doc, tidal, "refresh_token", tidal_refresh, sizeof(tidal_refresh));
	long expires = json_obj_long(&doc, tidal, "expires_at", -1);
	if (expires >= 0) {
		tidal_expires = expires;
	}
	if (!session) {
		take_str(&doc, tidal, "client_id", tidal_client_id, sizeof(tidal_client_id));
		take_str(&doc, tidal, "client_secret", tidal_client_secret, sizeof(tidal_client_secret));
	}

	json_free(&doc);
	wipe(text, len);
	free(text);
	printf("streamkeys: %s read (%s)\n", path, session ? "the player's session" : "the user's tokens");
	return true;
}

void streamkeys_attach_card(const char *sd_root) {
	if (!sd_root || !sd_root[0]) {
		printf("streamkeys: no card, the streaming accounts stay off\n");
		return;
	}
	snprintf(card_user_path, sizeof(card_user_path), "%s/%s/%s", sd_root, CARD_DIR_NAME, CARD_USER_FILE);
	snprintf(card_session_path, sizeof(card_session_path), "%s/%s/%s", sd_root, CARD_DIR_NAME,
			 CARD_SESSION_FILE);

	// The user's file first, the player's session over it: what the player saved
	// after a renewal is newer than what the user wrote by hand.
	read_card_json(card_user_path, false);
	read_card_json(card_session_path, true);

	printf("streamkeys: card -- Qobuz %s, Tidal %s\n", qobuz_token[0] ? "token" : "no token",
		   tidal_refresh[0] ? "token" : "no token");
}

static void write_escaped(FILE *f, const char *s) {
	for (const char *p = s; *p; p++) {
		if (*p == '"' || *p == '\\') {
			fputc('\\', f);
		}
		if ((unsigned char)*p < 0x20) {
			continue; // control characters have no place in a token
		}
		fputc(*p, f);
	}
}

static void write_session(void) {
	if (!card_session_path[0]) {
		return;
	}
	char tmp[560];
	snprintf(tmp, sizeof(tmp), "%s.tmp", card_session_path);

	FILE *f = fopen(tmp, "w");
	if (!f) {
		fprintf(stderr, "streamkeys: cannot write %s\n", tmp);
		return;
	}
	fprintf(f, "{\n");
	fprintf(f, "    \"qobuz\": {\"token\": \"");
	write_escaped(f, qobuz_token);
	fprintf(f, "\", \"user\": \"");
	write_escaped(f, qobuz_token_user);
	fprintf(f, "\", \"app_id\": \"");
	write_escaped(f, cached_app_id);
	fprintf(f, "\", \"app_secret\": \"");
	write_escaped(f, cached_app_secret);
	fprintf(f, "\", \"creds_at\": %ld},\n", cached_creds_at);
	fprintf(f, "    \"tidal\": {\"access_token\": \"");
	write_escaped(f, tidal_access);
	fprintf(f, "\", \"refresh_token\": \"");
	write_escaped(f, tidal_refresh);
	fprintf(f, "\", \"expires_at\": %ld}\n", tidal_expires);
	fprintf(f, "}\n");
	bool ok = fclose(f) == 0;
	if (!ok || rename(tmp, card_session_path) != 0) {
		fprintf(stderr, "streamkeys: the session was not saved\n");
		remove(tmp);
	}
}

void streamkeys_save_qobuz_session(const char *token, const char *user) {
	snprintf(qobuz_token, sizeof(qobuz_token), "%s", token ? token : "");
	snprintf(qobuz_token_user, sizeof(qobuz_token_user), "%s", user ? user : "");
	write_session();
}

void streamkeys_save_qobuz_creds(const char *app_id, const char *app_secret, long at) {
	snprintf(cached_app_id, sizeof(cached_app_id), "%s", app_id ? app_id : "");
	snprintf(cached_app_secret, sizeof(cached_app_secret), "%s", app_secret ? app_secret : "");
	cached_creds_at = at;
	write_session();
}

void streamkeys_save_tidal_session(const char *access_token, const char *refresh_token, long expires_at) {
	snprintf(tidal_access, sizeof(tidal_access), "%s", access_token ? access_token : "");
	snprintf(tidal_refresh, sizeof(tidal_refresh), "%s", refresh_token ? refresh_token : "");
	tidal_expires = expires_at;
	write_session();
}
