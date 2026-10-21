#include "qobuzkey.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "src/system/core/base64.h"
#include "src/system/net/http.h"

// The web player: the login page names the bundle every visitor is sent.
#define PLAYER_LOGIN "https://play.qobuz.com/login"
#define PLAYER_BASE "https://play.qobuz.com"

// The page is small; the bundle is not (about nine megabytes), and it is read
// as a stream -- see below.
#define LOGIN_LIMIT (512 * 1024)
#define FETCH_TIMEOUT_SECS 25

// How much of the end of one chunk is carried into the next scan: every pattern
// looked for here is shorter than this, so nothing straddling two reads is
// missed.
#define OVERLAP 512
#define CHUNK 32768

#define MAX_SEEDS 4
#define MAX_INFOS 8
#define TZ_MAX 16
#define PART_MAX 160

typedef struct {
	char tz[TZ_MAX];
	char seed[PART_MAX];
} seed_t;

typedef struct {
	char tz[TZ_MAX];
	char info[PART_MAX];
	char extras[PART_MAX];
} info_t;

typedef struct {
	seed_t seeds[MAX_SEEDS];
	int seed_count;
	info_t infos[MAX_INFOS];
	int info_count;
	char app_id[QOBUZKEY_ID_MAX];
} found_t;

// Copies `src` up to `stop` into `out`, never past its size. False when the
// span is empty or too long to be one of these parts.
static bool take_until(const char *src, const char *stop, char *out, size_t size) {
	size_t len = (size_t)(stop - src);
	if (len == 0 || len >= size || len >= PART_MAX) {
		return false;
	}
	memcpy(out, src, len);
	out[len] = '\0';
	return true;
}

// The timezone in `name:".../London"`: the word after the last slash.
static void take_timezone(const char *name_start, const char *quote, char *out, size_t size) {
	const char *slash = NULL;
	for (const char *p = name_start; p < quote; p++) {
		if (*p == '/') {
			slash = p;
		}
	}
	if (!slash) {
		out[0] = '\0';
		return;
	}
	size_t len = (size_t)(quote - (slash + 1));
	if (len == 0 || len >= size || len >= TZ_MAX) {
		out[0] = '\0';
		return;
	}
	memcpy(out, slash + 1, len);
	out[len] = '\0';
}

static void add_seed(found_t *f, const char *seed, const char *tz) {
	if (f->seed_count >= MAX_SEEDS || !seed[0] || !tz[0]) {
		return;
	}
	// The same timezone is seen again in every window it falls into; keeping one
	// entry per zone is what leaves room for the others.
	for (int i = 0; i < f->seed_count; i++) {
		if (strcasecmp(f->seeds[i].tz, tz) == 0) {
			return;
		}
	}
	snprintf(f->seeds[f->seed_count].tz, TZ_MAX, "%s", tz);
	snprintf(f->seeds[f->seed_count].seed, PART_MAX, "%s", seed);
	f->seed_count++;
}

static void add_info(found_t *f, const char *tz, const char *info, const char *extras) {
	if (f->info_count >= MAX_INFOS || !tz[0] || !info[0] || !extras[0]) {
		return;
	}
	for (int i = 0; i < f->info_count; i++) {
		if (strcasecmp(f->infos[i].tz, tz) == 0) {
			return;
		}
	}
	snprintf(f->infos[f->info_count].tz, TZ_MAX, "%s", tz);
	snprintf(f->infos[f->info_count].info, PART_MAX, "%s", info);
	snprintf(f->infos[f->info_count].extras, PART_MAX, "%s", extras);
	f->info_count++;
}

// One pass over a window of the bundle. Everything searched for here is a local
// pattern of at most a few hundred bytes, so a window plus the overlap carried
// between reads is enough to see all of them.
static void scan_window(found_t *f, const char *buf, size_t len) {
	for (size_t i = 0; i + 14 < len; i++) {
		// Only the id under "production:{api:{" is the one the API takes: the
		// bundle names several applications for its other services.
		if (!f->app_id[0] && i + 24 < len && memcmp(buf + i, "production:{api:{appId:\"", 24) == 0) {
			const char *p = buf + i + 24;
			int digits = 0;
			while (digits < 9 && p + digits < buf + len && p[digits] >= '0' && p[digits] <= '9') {
				digits++;
			}
			if (digits == 9) {
				memcpy(f->app_id, p, 9);
				f->app_id[9] = '\0';
			}
			continue;
		}

		if (memcmp(buf + i, "initialSeed(\"", 13) == 0) {
			const char *seed_start = buf + i + 13;
			const char *seed_end = memchr(seed_start, '"', (size_t)(buf + len - seed_start));
			if (!seed_end) {
				continue;
			}
			char seed[PART_MAX];
			if (!take_until(seed_start, seed_end, seed, sizeof(seed))) {
				continue;
			}
			// The timezone follows within a few characters: ", window.utimezone.<tz>)".
			const char *tz_point = NULL;
			for (const char *p = seed_end; p + 10 < buf + len && p < seed_end + 80; p++) {
				if (memcmp(p, "utimezone.", 10) == 0) {
					tz_point = p + 10;
					break;
				}
			}
			if (!tz_point) {
				continue;
			}
			const char *tz_end = tz_point;
			while (tz_end < buf + len && *tz_end >= 'a' && *tz_end <= 'z' && tz_end - tz_point < TZ_MAX - 1) {
				tz_end++;
			}
			char tz[TZ_MAX];
			if (!take_until(tz_point, tz_end, tz, sizeof(tz))) {
				continue;
			}
			add_seed(f, seed, tz);
			continue;
		}

		if (memcmp(buf + i, ",info:\"", 7) == 0) {
			const char *info_start = buf + i + 7;
			const char *info_end = memchr(info_start, '"', (size_t)(buf + len - info_start));
			if (!info_end) {
				continue;
			}
			if (memcmp(info_end, "\",extras:\"", 10) != 0) {
				continue;
			}
			const char *extras_start = info_end + 10;
			const char *extras_end = memchr(extras_start, '"', (size_t)(buf + len - extras_start));
			if (!extras_end) {
				continue;
			}
			// The name that precedes it carries the timezone.
			const char *name = NULL;
			for (const char *p = buf + i; p > buf && p > buf + i - 200; p--) {
				if (p - buf >= 6 && memcmp(p - 6, "name:\"", 6) == 0) {
					name = p;
					break;
				}
			}
			if (!name) {
				continue;
			}
			const char *quote = memchr(name, '"', (size_t)(buf + len - name));
			if (!quote || quote > info_start) {
				continue;
			}
			char tz[TZ_MAX];
			take_timezone(name, quote, tz, sizeof(tz));
			char info[PART_MAX], extras[PART_MAX];
			if (!take_until(info_start, info_end, info, sizeof(info))) {
				continue;
			}
			if (!take_until(extras_start, extras_end, extras, sizeof(extras))) {
				continue;
			}
			add_info(f, tz, info, extras);
		}
	}
}

// The secret of one timezone: seed + info + extras, the last 44 characters
// dropped, base64-decoded.
static bool derive_secret(const char *seed, const char *info, const char *extras, char *out, size_t size) {
	char joined[PART_MAX * 3];
	int n = snprintf(joined, sizeof(joined), "%s%s%s", seed, info, extras);
	if (n <= 0 || (size_t)n >= sizeof(joined) || n <= 44) {
		return false;
	}

	size_t decoded = 0;
	uint8_t *raw = base64_decode(joined, (size_t)n - 44, PART_MAX, &decoded);
	if (!raw) {
		return false;
	}
	bool ok = decoded > 16 && decoded < size && decoded < PART_MAX;
	if (ok) {
		memcpy(out, raw, decoded);
		out[decoded] = '\0';
		// Only printable ASCII belongs in a secret; anything else means this
		// candidate is not one.
		for (size_t i = 0; i < decoded; i++) {
			if (out[i] < 0x21 || out[i] > 0x7e) {
				ok = false;
				break;
			}
		}
	}
	free(raw);
	if (!ok) {
		out[0] = '\0';
	}
	return ok;
}

static void add_candidate(qobuzkey_t *out, const char *secret) {
	if (!secret[0]) {
		return;
	}
	for (int i = 0; i < out->count; i++) {
		if (strcmp(out->secrets[i], secret) == 0) {
			return;
		}
	}
	if (out->count >= QOBUZKEY_MAX_CANDIDATES) {
		return;
	}
	snprintf(out->secrets[out->count], QOBUZKEY_SECRET_MAX, "%s", secret);
	out->count++;
}

bool qobuzkey_fetch(qobuzkey_t *out) {
	memset(out, 0, sizeof(*out));

	char *page = NULL;
	size_t page_len = 0;
	if (!http_get(PLAYER_LOGIN, &page, &page_len, LOGIN_LIMIT, FETCH_TIMEOUT_SECS)) {
		printf("qobuzkey: %s did not answer (%s)\n", PLAYER_LOGIN, http_last_error());
		return false;
	}

	// Where the page says its bundle is: /resources/<version>/bundle.js.
	char bundle[256];
	bundle[0] = '\0';
	const char *res = strstr(page, "/resources/");
	if (res) {
		const char *end = strstr(res, "bundle.js");
		if (end && (size_t)(end + 9 - res) < sizeof(bundle)) {
			size_t len = (size_t)(end + 9 - res);
			memcpy(bundle, res, len);
			bundle[len] = '\0';
		}
	}
	free(page);
	if (!bundle[0]) {
		printf("qobuzkey: the login page names no bundle\n");
		return false;
	}

	char url[512];
	snprintf(url, sizeof(url), "%s%s", PLAYER_BASE, bundle);

	http_stream_t stream;
	if (!http_stream_open(&stream, url, FETCH_TIMEOUT_SECS)) {
		printf("qobuzkey: %s did not open (%s)\n", url, http_last_error());
		return false;
	}
	printf("qobuzkey: reading %s\n", bundle);

	found_t *found = calloc(1, sizeof(*found));
	char *buf = malloc(CHUNK + OVERLAP + 1);
	if (!found || !buf) {
		free(found);
		free(buf);
		http_stream_close(&stream);
		return false;
	}

	size_t carry = 0;
	long total = 0;
	for (;;) {
		int got = http_stream_read(&stream, buf + carry, CHUNK);
		if (got <= 0) {
			break;
		}
		size_t len = carry + (size_t)got;
		total += got;
		scan_window(found, buf, len);

		// Carry the tail into the next read, and start the next scan from the
		// part of it that has not been looked at.
		carry = len < OVERLAP ? len : OVERLAP;
		memmove(buf, buf + len - carry, carry);
	}
	http_stream_close(&stream);

	printf("qobuzkey: %ld bytes, app id %s, %d seed(s), %d info pair(s)\n", total,
		   found->app_id[0] ? found->app_id : "(none)", found->seed_count, found->info_count);

	if (found->app_id[0]) {
		snprintf(out->app_id, sizeof(out->app_id), "%s", found->app_id);
	}

	for (int i = 0; i < found->seed_count; i++) {
		for (int k = 0; k < found->info_count; k++) {
			if (strcasecmp(found->seeds[i].tz, found->infos[k].tz) != 0) {
				continue;
			}
			char secret[QOBUZKEY_SECRET_MAX];
			if (derive_secret(found->seeds[i].seed, found->infos[k].info, found->infos[k].extras, secret,
							  sizeof(secret))) {
				add_candidate(out, secret);
			}
		}
	}

	free(buf);
	free(found);

	if (!out->app_id[0] || out->count == 0) {
		printf("qobuzkey: the bundle carried no usable credentials\n");
		return false;
	}
	return true;
}
