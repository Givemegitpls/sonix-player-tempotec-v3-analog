#ifndef QOBUZKEY_H
#define QOBUZKEY_H

#include <stdbool.h>
#include <stddef.h>

// Qobuz's application credentials, taken from its own web player at run time.
//
// The API needs an application id and a secret: the id goes in every request,
// the secret signs the calls that hand out stream URLs. They are Qobuz's, not
// this project's, so nothing of them is in the image -- the player asks the web
// player for them the way qobuz-dl does, and keeps the answer on the card.
//
// The trick is in what the player's JavaScript bundle carries. Alongside the
// appSecret written plainly (which the API refuses to sign with) it embeds, per
// timezone, a seed, an "info" and an "extras" string:
//
//     a.london.initialSeed("...", window.utimezone.london)
//     name:".../London",info:"...",extras:"..."
//
// Concatenating the three, dropping the last 44 characters and base64-decoding
// the rest gives a secret. Which of the timezone's three candidates the API
// takes changes without notice, so all of them are returned and the caller
// tries them.
//
// This is the same derivation the open-source downloaders do; it asks for two
// public files and no account.

#define QOBUZKEY_MAX_CANDIDATES 6
#define QOBUZKEY_ID_MAX 32
#define QOBUZKEY_SECRET_MAX 64

typedef struct {
	char app_id[QOBUZKEY_ID_MAX];
	char secrets[QOBUZKEY_MAX_CANDIDATES][QOBUZKEY_SECRET_MAX];
	int count;
} qobuzkey_t;

// Fetches the bundle and derives the credentials. Blocking, so never from the
// UI thread. False when the player could not be reached or nothing was found --
// a network failure is not a crash, and the caller decides what to say.
bool qobuzkey_fetch(qobuzkey_t *out);

#endif /* QOBUZKEY_H */
